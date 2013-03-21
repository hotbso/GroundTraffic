/*
 * GroundTraffic
 *
 * (c) Jonathan Harris 2013
 *
 * Licensed under GNU LGPL v2.1.
 */

#include "groundtraffic.h"

#define N(c) (c?c:"<nothing>")

/* Globals */
static time_t mtime=-1;	/* control file modification time  */
static const char sep[]=" \t\r\n";

/* In this file */
static userref_t *readuserref(airport_t *airport, route_t *currentroute, char *buffer, int lineno);
static route_t *expandtrain(airport_t *airport, route_t *currentroute);

const glColor3f_t colors[16] = { { 0.0, 1.0, 0.0 }, // lime (match DRE color)
                                 { 1.0, 0.0, 0.0 }, // red
                                 { 1.0, 1.0, 0.0 }, // yellow
                                 { 0.0, 0.0, 1.0 }, // blue
                                 { 0.0, 1.0, 1.0 }, // aqua
                                 { 1.0, 0.0, 1.0 }, // fuchsia
                                 { 1.0,0.65, 1.0 }, // orange
                                 { 0.5, 0.5, 0.5 }, // gray
                                 { 0.5, 0.0, 0.0 }, // maroon
                                 { 0.5, 0.5, 0.0 }, // olive
                                 { 0.0, 0.5, 0.0 }, // green
                                 { 0.0, 0.5, 0.5 }, // teal
                                 { 0.0, 0.0, 0.5 }, // navy
                                 { 0.5, 0.0, 0.5 }, // purple
                                 {0.75,0.75,0.75 }, // silver
                                 { 0.0, 0.0, 0.0 }, // black
};

void clearconfig(airport_t *airport)
{
    route_t *route;
    train_t *train;
    userref_t *userref;
    extref_t *extref;

    deactivate(airport);

    airport->ICAO[0]='\0';
    airport->tower.lat=airport->tower.lon=0;
    airport->tower.alt=INVALID_ALT;
    airport->state = noconfig;
    airport->drawroutes = 0;

    route = airport->routes;
    while (route)
    {
        route_t *next = route->next;
        if (!route->parent)	/* Paths are shared with parent */
        {
            int i;
            for (i=0; i<route->pathlen; i++)
            {
                collision_t *nextc, *collision = route->path[i].collisions;
                while (collision)
                {
                    nextc = collision->next;
                    free (collision);
                    collision = nextc;
                }
            }
            free(route->path);
        }
        free(route);
        route = next;
    }
    airport->routes = airport->firstroute = NULL;

    train = airport->trains;
    while (train)
    {
        train_t *next = train->next;
        free(train);
        train = next;
    }
    airport->trains = NULL;

    userref = airport->userrefs;
    while (userref)
    {
        userref_t *next = userref->next;
        if (userref->ref)
            XPLMUnregisterDataAccessor(userref->ref);
        free(userref->name);
        free(userref);
        userref = next;
    }
    airport->userrefs = NULL;

    extref = airport->extrefs;
    while (extref)
    {
        extref_t *next = extref->next;
        free(extref->name);
        free(extref);
        extref = next;
    }
    airport->extrefs = NULL;

    free(airport->drawinfo);
    airport->drawinfo = NULL;

    mtime=-1;		/* Don't cache */
}   

/* Convenience function */
static int failconfig(FILE *h, airport_t *airport, char *buffer, const char *format, ...)
{
    va_list ap;

    va_start(ap, format);
    vsprintf(buffer, format, ap);
    va_end(ap);
    xplog(buffer);
    clearconfig(airport);
    fclose(h);
    return 1;
}

/* 
 * Read our config file
 * Return: 0=config hasn't changed, !0=config has changed and airport->state is updated
 */
int readconfig(char *pkgpath, airport_t *airport)
{
    struct stat info;
    char buffer[MAX_NAME+128], line[MAX_NAME+64];
    FILE *h;
    int lineno=0, count=0;
    route_t *lastroute=NULL, *currentroute=NULL;
    train_t *lasttrain=NULL, *currenttrain=NULL;
    userref_t *userref;

#if APL || LIN		/* Might be a case sensitive file system */
    DIR *dir;
    struct dirent *ent;

    *buffer='\0';
    if (!(dir=opendir(pkgpath)))
    {
        clearconfig(airport);
        xplog("Can't find my scenery folder");
        return 1;
    }
    while ((ent=readdir(dir)))
        if (!strcasecmp(ent->d_name, "groundtraffic.txt"))
        {
            strcpy(buffer, pkgpath);
            strcat(buffer, "/");
            strcat(buffer, ent->d_name);
            break;
        }
    closedir(dir);
    if (!*buffer)
    {
        clearconfig(airport);
        sprintf(buffer, "Can't find groundtraffic.txt in %s", pkgpath);
        xplog(buffer);
        return 1;
    }
#else	/* Windows uses a case folding file system */
    strcpy(buffer, pkgpath);
    strcat(buffer, "/groundtraffic.txt");
#endif

    if (stat(buffer, &info))
    {
        clearconfig(airport);
        sprintf(buffer, "Can't find groundtraffic.txt in %s", pkgpath);
        xplog(buffer);
        return 1;
    }
    if (info.st_mtime==mtime) return 0;		/* File hasn't changed */
    clearconfig(airport);			/* File has changed - free old config */

    if (!(h=fopen(buffer, "r")))
    {
        sprintf(buffer, "Can't open %s/groundtraffic.txt", pkgpath);
        xplog(buffer);
        return 1;
    }
    while (fgets(line, sizeof(line)-1, h))
    {
        char *c1, *c2, *c3;
        int eol1, eol2, eol3;

        line[strlen(line)+1]='\0';	/* Prevent potential overrun if missing eol on last line */
        if (!lineno && !strncmp(line, "\xef\xbb\xbf", 3))	/* skip UTF-8 BOM */
            c1=strtok(line+3, sep);
        else
            c1=strtok(line, sep);
        lineno++;

        if (!c1)				/* Blank line = end of route or train */
        {
            if (currentroute && !currentroute->pathlen)
                return failconfig(h, airport, buffer, "Empty route at line %d", lineno);
            currentroute = NULL;
            if (currenttrain && !currenttrain->objects[0].name[0])
                return failconfig(h, airport, buffer, "Empty train at line %d", lineno);
            currenttrain = NULL;
            continue;
        }
        else if (*c1=='#')			/* Skip comment lines */
        {
            continue;
        }
        else if (airport->state==noconfig)	/* Airport header */
        {
            if (strlen(c1)==4)
                strcpy(airport->ICAO, c1);
            else
                return failconfig(h, airport, buffer, "Expecting a 4 character airport ICAO code, found \"%s\" at line %d", c1, lineno);
            c1=strtok(NULL, sep);
            c2=strtok(NULL, sep);
            if (!c1 || !sscanf(c1, "%f%n", &airport->tower.lat, &eol1) || c1[eol1] ||
                !c2 || !sscanf(c2, "%f%n", &airport->tower.lon, &eol2) || c2[eol2])
                return failconfig(h, airport, buffer, "Expecting an airport location \"lat lon\", found \"%s %s\" at line %d", N(c1), N(c2), lineno);
            if ((c1=strtok(NULL, sep)))
                return failconfig(h, airport, buffer, "Extraneous input \"%s\" at line %d", c1, lineno);

            airport->state=inactive;
        }
        else if (currentroute)			/* Existing route */
        {
            path_t *path;
            if (!(path=realloc(currentroute->path, (1+currentroute->pathlen) * sizeof(path_t))))
                return failconfig(h, airport, buffer, "Out of memory!");
            currentroute->path=path;

            /* Note: currentroute->pathlen hasn't been updated yet so points to the newly allocated path */
            if (!strcasecmp(c1, "pause"))
            {
                int pausetime;
                if (!currentroute->pathlen)
                    return failconfig(h, airport, buffer, "Route can't start with a pause command at line %d", lineno);

                c1=strtok(NULL, sep);
                if (!c1 || !sscanf(c1, "%d%n", &pausetime, &eol1) || c1[eol1])
                    return failconfig(h, airport, buffer, "Expecting a pause time, found \"%s\" at line %d", N(c1), lineno);
                else if (pausetime <= 0 || pausetime >= 86400)
                    return failconfig(h, airport, buffer, "Pause time should be between 1 and 86399 seconds at line %d", lineno);
                path[currentroute->pathlen-1].pausetime += pausetime;	/* Multiple pauses stack */

                if ((c1=strtok(NULL, sep)))
                {
                    if (strcasecmp(c1, "set"))
                        return failconfig(h, airport, buffer, "Expecting \"set\" or nothing, found \"%s\" at line %d", c1, lineno);
                    else if ((path[currentroute->pathlen-1].userref = readuserref(airport, currentroute, buffer, lineno)))
                    {
                        path[currentroute->pathlen-1].flags.set2=1;
                    }
                    else
                    {
                        fclose(h);
                        clearconfig(airport);
                        xplog(buffer);
                        return 1;
                    }
                }
            }
            else if (!strcasecmp(c1, "at"))
            {
                int hour, minute, i=0;
                char daynames[7][10] = { "sunday", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday" };
                int dayvals[7] = { DAY_SUN, DAY_MON, DAY_TUE, DAY_WED, DAY_THU, DAY_FRI, DAY_SAT };

                if (!currentroute->pathlen)
                    return failconfig(h, airport, buffer, "Route can't start with an \"at\" command at line %d", lineno);
                else if (path[currentroute->pathlen-1].attime[0] != INVALID_AT)
                    return failconfig(h, airport, buffer, "Waypoint can't have more than one \"at\" command at line %d", lineno);
                while ((c1=strtok(NULL, sep)))
                {
                    if (!strcasecmp(c1, "on"))
                        break;
                    else if (i>=MAX_ATTIMES)
                        return failconfig(h, airport, buffer, "Exceeded %d times-of-day at line %d", MAX_ATTIMES, lineno);
                    else if (sscanf(c1, "%d:%d%n", &hour, &minute, &eol1)!=2 || c1[eol1] || hour<0 || hour>23 || minute<0 || minute>59)
                        return failconfig(h, airport, buffer, "Expecting a time-of-day \"HH:MM\" or \"on\", found \"%s\" at line %d", c1, lineno);
                    path[currentroute->pathlen-1].attime[i++] = hour*60+minute;
                }
                if (i<MAX_ATTIMES) path[currentroute->pathlen-1].attime[i] = INVALID_AT;	/* Terminate */

                while ((c1=strtok(NULL, sep)))
                {
                    for (i=0; i<7; i++)
                        if (!strncasecmp(c1, daynames[i], strlen(c1)))
                        {
                            path[currentroute->pathlen-1].atdays |= dayvals[i];
                            break;
                        }
                    if (i>=7)
                        return failconfig(h, airport, buffer, "Expecting a day name, found \"%s\" at line %d", c1, lineno);
                }
                if (!path[currentroute->pathlen-1].atdays) path[currentroute->pathlen-1].atdays = DAY_ALL;
            }
            else if (!strcasecmp(c1, "when"))
            {
                extref_t *extref;

                if (!currentroute->pathlen)
                    return failconfig(h, airport, buffer, "Route can't start with a \"when\" command at line %d", lineno);
                else if (path[currentroute->pathlen-1].whenref)
                    return failconfig(h, airport, buffer, "Waypoint can't have more than one \"when\" command at line %d", lineno);

                if (!(c1=strtok(NULL, sep)))
                    return failconfig(h, airport, buffer, "Expecting a DataRef name at line %d", c1, lineno);

                if ((c2 = strchr(c1, '[')))
                {
                    *(c2++) = '\0';	/* Strip index for lookup */
                    if (!sscanf(c2, "%d%n", &path[currentroute->pathlen-1].whenidx, &eol2) || eol2!=strlen(c2)-1 || c2[eol2]!=']')
                        return failconfig(h, airport, buffer, "Expecting a DataRef index \"[n]\", found \"[%s\" at line %d", N(c2), lineno);
                    else if (path[currentroute->pathlen-1].whenidx < 0)
                        return failconfig(h, airport, buffer, "DataRef index cannot be negative at line %d", lineno);
                }
                else
                    path[currentroute->pathlen-1].whenidx = -1;

                for (extref = airport->extrefs; extref && strcmp(c1, extref->name); extref=extref->next);
                if (!extref)
                {
                    /* new */
                    if (!(extref = calloc(1, sizeof(extref_t))) || !(extref->name = malloc(strlen(c1)+1)))
                        return failconfig(h, airport, buffer, "Out of memory!");
                    strcpy(extref->name, c1);	/* Defer lookup to activation, after other plugins have Enabled */
                    extref->next = airport->extrefs;
                    airport->extrefs = extref;
                }
                path[currentroute->pathlen-1].whenref = extref;

                c1=strtok(NULL, sep);
                c2=strtok(NULL, sep);
                if (!c1 || !sscanf(c1, "%f%n", &path[currentroute->pathlen-1].whenfrom, &eol1) || c1[eol1] ||
                    !c2 || !sscanf(c2, "%f%n", &path[currentroute->pathlen-1].whento,   &eol2) || c2[eol2])
                    return failconfig(h, airport, buffer, "Expecting a range \"from to\", found \"%s %s\" at line %d", N(c1), N(c2), lineno);
                if (path[currentroute->pathlen-1].whenfrom > path[currentroute->pathlen-1].whento)
                {
                    float foo = path[currentroute->pathlen-1].whenfrom;
                    path[currentroute->pathlen-1].whenfrom = path[currentroute->pathlen-1].whento;
                    path[currentroute->pathlen-1].whento = foo;
                }
            }
            else if (!strcasecmp(c1, "backup"))
            {
                if (!currentroute->pathlen)
                    return failconfig(h, airport, buffer, "Route can't start with a backup command at line %d", lineno);
                else if (currentroute->pathlen>1 && path[currentroute->pathlen-2].flags.backup)
                    return failconfig(h, airport, buffer, "Can't backup from two waypoints in sequence at line %d", lineno);

                path[currentroute->pathlen-1].flags.backup=1;
            }
            else if (!strcasecmp(c1, "reverse"))
            {
                int i;
                if (!currentroute->pathlen)
                    return failconfig(h, airport, buffer, "Empty route at line %d", lineno);
                for (i=0; i<currentroute->pathlen; i++)
                    if (path[i].flags.backup)
                        return failconfig(h, airport, buffer, "Can't use \"backup\" and \"reverse\" in the same route at line %d", lineno);
                path[currentroute->pathlen-1].flags.reverse=1;
                currentroute=NULL;		/* reverse terminates */
            }
            else if (!strcasecmp(c1, "set"))
            {
                if ((path[currentroute->pathlen-1].userref = readuserref(airport, currentroute, buffer, lineno)))
                {
                    path[currentroute->pathlen-1].flags.set1=1;
                }
                else
                {
                    fclose(h);
                    clearconfig(airport);
                    xplog(buffer);
                    return 1;
                }
            }
            else				/* waypoint */
            {
                memset(path + currentroute->pathlen, 0, sizeof(path_t));
                path[currentroute->pathlen].waypoint.alt = INVALID_ALT;
                path[currentroute->pathlen].attime[0] = INVALID_AT;
                c2=strtok(NULL, sep);
                if (!c1 || !sscanf(c1, "%f%n", &path[currentroute->pathlen].waypoint.lat, &eol1) || c1[eol1] ||
                    !c2 || !sscanf(c2, "%f%n", &path[currentroute->pathlen].waypoint.lon, &eol2) || c2[eol2])
                    return failconfig(h, airport, buffer, "Expecting a waypoint \"lat lon\", a command or a blank line, found \"%s %s\" at line %d", N(c1), N(c2), lineno);

                currentroute->pathlen++;
            }
            if ((c1=strtok(NULL, sep))) return failconfig(h, airport, buffer, "Extraneous input \"%s\" at line %d", c1, lineno);
        }
        else if (currenttrain)			/* Existing train */
        {
            int n;	/* Train length */

            for (n=0; n<MAX_TRAIN && currenttrain->objects[n].name[0]; n++);
            if (n>=MAX_TRAIN)
                return failconfig(h, airport, buffer, "Exceeded %d objects in a train at line %d", MAX_TRAIN, lineno);

            c2=strtok(NULL, sep);
            c3=strtok(NULL, sep);
            if (!c1 || !sscanf(c1, "%f%n", &currenttrain->objects[n].lag, &eol1) || c1[eol1] ||
                !c2 || !sscanf(c2, "%f%n", &currenttrain->objects[n].offset, &eol2) || c2[eol2] ||
                !c3 || !sscanf(c3, "%f%n", &currenttrain->objects[n].heading, &eol3) || c3[eol3])
                return failconfig(h, airport, buffer, "Expecting a car \"lag offset heading\" or a blank line, found \"%s %s %s\" at line %d", N(c1), N(c2), N(c3), lineno);

            for (c1 = c3+strlen(c3)+1; isspace(*c1); c1++);		/* ltrim */
            for (c2 = c1+strlen(c1)-1; isspace(*c2); *(c2--) = '\0');	/* rtrim */
            if (c1==c2)
                return failconfig(h, airport, buffer, "Expecting an object name at line %d", lineno);
            else if (strlen(c1) >= MAX_NAME)
                return failconfig(h, airport, buffer, "Object name exceeds %d characters at line %d", MAX_NAME-1, lineno);
            else
                strcpy(currenttrain->objects[n].name, c1);
        }
        else if (!strcasecmp(c1, "route"))	/* New route */
        {
            route_t *newroute;
            if (!(newroute=calloc(1, sizeof(route_t))))
                return failconfig(h, airport, buffer, "Out of memory!");
            else if (lastroute)
                lastroute->next = newroute;
            else
                airport->routes = airport->firstroute = newroute;	/* Save for DRE */

            /* Initialise the route */
            newroute->direction = 1;
            if (count<16)
            {
                newroute->drawcolor = colors[count];
            }
            else
            {
                newroute->drawcolor.r = ((float) rand()) / RAND_MAX;
                newroute->drawcolor.g = ((float) rand()) / RAND_MAX;
                newroute->drawcolor.b = ((float) rand()) / RAND_MAX;
            }
            count++;

            c1=strtok(NULL, sep);
            c2=strtok(NULL, sep);
            c3=strtok(NULL, sep);
            if (!c1 || !sscanf(c1, "%f%n", &newroute->speed, &eol1) || c1[eol1] ||
                !c2 || !sscanf(c2, "%f%n", &newroute->object.offset, &eol2) || c2[eol2] ||
                !c3 || !sscanf(c3, "%f%n", &newroute->object.heading, &eol3) || c3[eol3])
                return failconfig(h, airport, buffer, "Expecting a route \"speed offset heading\", found \"%s %s %s\" at line %d",  N(c1), N(c2), N(c3), lineno);

            for (c1 = c3+strlen(c3)+1; isspace(*c1); c1++);		/* ltrim */
            for (c2 = c1+strlen(c1)-1; isspace(*c2); *(c2--) = '\0');	/* rtrim */
            if (c1==c2)
                return failconfig(h, airport, buffer, "Expecting an object name at line %d", lineno);
            else if (strlen(c1) >= MAX_NAME)
                return failconfig(h, airport, buffer, "Object name exceeds %d characters at line %d", MAX_NAME-1, lineno);
            else
                strcpy(newroute->object.name, c1);

            newroute->speed *= (1000.0 / (60*60));	/* convert km/h to m/s */
            currentroute=lastroute=newroute;
        }
        else if (!strcasecmp(c1, "train"))	/* New train */
        {
            train_t *newtrain;
            if (!(newtrain=calloc(1, sizeof(train_t))))
                return failconfig(h, airport, buffer, "Out of memory!");
            else if (lasttrain)
                lasttrain->next=newtrain;
            else
                airport->trains=newtrain;

            for (c1 = c1+strlen(c1)+1; isspace(*c1); c1++);		/* ltrim */
            for (c2 = c1+strlen(c1)-1; isspace(*c2); *(c2--) = '\0');	/* rtrim */
            if (c1==c2)
                return failconfig(h, airport, buffer, "Expecting a train name at line %d", lineno);
            else if (strlen(c1) >= MAX_NAME)
                return failconfig(h, airport, buffer, "Train name exceeds %d characters at line %d", MAX_NAME-1, lineno);
            else
                strcpy(newtrain->name, c1);

            currenttrain=lasttrain=newtrain;
        }
        else if (!strcasecmp(c1, "debug"))
        {
            airport->drawroutes = -1;
            if ((c1=strtok(NULL, sep))) return failconfig(h, airport, buffer, "Extraneous input \"%s\" at line %d", c1, lineno);
        }
        else
        {
            return failconfig(h, airport, buffer, "Expecting a route or train, found \"%s\" at line %d", c1, lineno);
        }
    }

    /* Turn train routes into multiple individual routes */
    currentroute = airport->routes;
    while (currentroute)
    {
        if (!(currentroute = expandtrain(airport, currentroute)))
            return failconfig(h, airport, buffer, "Out of memory!");
        currentroute = currentroute->next;
    }

    /* Allocate XPLMDrawInfo_t array. We don't assign into this 'til XPLMObjectRefs are known during activate() */
    for (count = 0, currentroute = airport->routes; currentroute; count++, currentroute = currentroute->next);
    if (!(airport->drawinfo = calloc(count, sizeof(XPLMDrawInfo_t))))
        return failconfig(h, airport, buffer, "Out of memory!");
    for (count = 0, currentroute = airport->routes; currentroute; count++, currentroute = currentroute->next)
        airport->drawinfo[count].structSize = sizeof(XPLMDrawInfo_t);
    
    /* Register user's DataRefs.
     * Have to do this early rather than during activate() because objects in DSF are loaded while we're still inactive */
    userref = airport->userrefs;
    while (userref)
    {
        if (XPLMFindDataRef(userref->name))
            return failconfig(h, airport, buffer, "Another plugin has already registered custom DataRef \"%s\"", userref->name);
        userref->ref = XPLMRegisterDataAccessor(userref->name, xplmType_Float, 0,
                                                NULL, NULL, userrefcallback, NULL, NULL, NULL,
                                                NULL, NULL, NULL, NULL, NULL, NULL, userref, NULL);
        userref = userref->next;
    }

    if (airport->state==noconfig)
        return failconfig(h, airport, buffer, "Can't read groundtraffic.txt");
    else if (!airport->routes)
        return failconfig(h, airport, buffer, "No routes defined!");

    fclose(h);
    mtime=info.st_mtime;
    return 2;
}

/* Read standalone or pause "set" command. Returns NULL on failure, and leaves error message in buffer. */
static userref_t *readuserref(airport_t *airport, route_t *currentroute, char *buffer, int lineno)
{
    userref_t *userref;
    char *c1;
    int eol1;

    if (!(c1=strtok(NULL, sep)))
    {
        sprintf(buffer, "Expecting a DataRef name at line %d", lineno);
        return 0;
    }
    else if (strlen(c1) >= MAX_NAME)
    {
        sprintf(buffer, "DataRef name exceeds %d characters at line %d", MAX_NAME-1, lineno);
        return 0;
    }

    if ((!strncasecmp(c1, "var[", 4) || !strncasecmp(c1, REF_VAR "[", sizeof(REF_VAR "["))) && c1[strlen(c1)-1]==']')
    {
        /* Standard DataRef = route-specific */
        int i;
        c1 = strchr(c1, '[') + 1;
        if (!sscanf(c1, "%d%n", &i, &eol1) || eol1!=strlen(c1)-1)
        {
            sprintf(buffer, "Expecting DataRef name \"var[n]\", found \"%s\" at line %d", N(c1), lineno);
            return 0;
        }
        else if (i<0 || i>=MAX_VAR)
        {
            sprintf(buffer, "var DataRef index outside the range 0 to %d at line %d", MAX_VAR-1, lineno);
            return 0;
        }
        userref = currentroute->varrefs + i;
    }
    else
    {
        /* User DataRef = global */
        for(userref = airport->userrefs; userref && strcmp(c1, userref->name); userref=userref->next);
        if (!userref)
        {
            /* new */
            if (!strncasecmp(c1, "sim/", 4))
            {
                sprintf(buffer, "Custom DataRef name can't start with \"sim/\" at line %d", lineno);
                return 0;
            }
            else if (!strncasecmp(c1, "marginal/", 9))
            {
                sprintf(buffer, "Custom DataRef name can't start with \"marginal/\", invent your own name! at line %d", lineno);
                return 0;
            }
            else if (!(userref = calloc(1, sizeof(userref_t))) || !(userref->name = malloc(strlen(c1)+1)))
            {
                strcpy(buffer, "Out of memory!");
                return 0;
            }
            strcpy(userref->name, c1);
            userref->next = airport->userrefs;
            airport->userrefs = userref;
        }
    }

    c1=strtok(NULL, sep);
    if (c1 && !strcasecmp(c1, "rise"))
        userref->slope = rising;
    else if (c1 && !strcasecmp(c1, "fall"))
        userref->slope = falling;
    else
    {
        sprintf(buffer, "Expecting a slope \"rise\" or \"fall\", found \"%s\" at line %d", N(c1), lineno);
        return 0;
    }

    c1=strtok(NULL, sep);
    if (c1 && !strcasecmp(c1, "linear"))
        userref->curve = linear;
    else if (c1 && !strcasecmp(c1, "sine"))
        userref->curve = sine;
    else
    {
        sprintf(buffer, "Expecting a curve \"linear\" or \"sine\", found \"%s\" at line %d", N(c1), lineno);
        return 0;
    }

    c1=strtok(NULL, sep);
    if (!c1 || !sscanf(c1, "%f%n", &userref->duration, &eol1) || c1[eol1])
    {
        sprintf(buffer, "Expecting a duration, found \"%s\" at line %d", N(c1), lineno);
        return 0;
    }

    return userref;
}


/* Check if this route names a train; if so replicate into multiple routes, and return pointer to last */
static route_t *expandtrain(airport_t *airport, route_t *currentroute)
{
    int i;
    train_t *train = airport->trains;
    route_t *route = currentroute;

    assert (currentroute);
    if (!currentroute) return NULL;

    while (train)
    {
        if (!strcmp(currentroute->object.name, train->name)) break;
        train = train->next;
    }
    if (!train) return currentroute;

    /* It's a train */
    for (i=0; i<MAX_TRAIN; i++)
    {
        if (!train->objects[i].name[0]) break;
        if (i)
        {
            /* Duplicate original route */
            route_t *newroute;
            if (!(newroute=malloc(sizeof(route_t)))) return NULL; /* OOM */
            memcpy(newroute, currentroute, sizeof(route_t));
            newroute->next = route->next;
            route->next = newroute;
            route = newroute;
            route->parent = currentroute;
        }
        /* Assign carriage to its route */
        strcpy(route->object.name, train->objects[i].name);
        route->object.lag = train->objects[i].lag / route->speed;	/* Convert distance to time lag */
        route->object.offset = train->objects[i].offset;
        route->object.heading = train->objects[i].heading;
        route->next_time = -route->object.lag;				/* Force recalc on first draw */
    }

    return route;
}
