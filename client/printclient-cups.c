#include <stdio.h>
#include <glib.h>
#include <cups/cups.h>
#include "printclient.h"
#include "PPDGenerator.h"


static void openWithApp(const char * file) {
    char command[1024];
    snprintf(command, 1024, "xdg-open %s", file);
    // TODO: on Mac OS X, the command is 'open'
    system(command);
}


int flexvdiSpiceGetPrinterList(GSList ** printerList) {
    int i;
    cups_dest_t * dests, * dest;
    int numDests = cupsGetDests(&dests);
    char instance[256];
    *printerList = NULL;
    for (i = numDests, dest = dests; i > 0; --i, ++dest) {
        char * name;
        if (dest->instance) {
            g_snprintf(instance, 256, "%s/%s\n", dest->name, dest->instance);
            *printerList = g_slist_prepend(*printerList, g_strdup(instance));
        } else {
            *printerList = g_slist_prepend(*printerList, g_strdup(dest->name));
        }
    }
    cupsFreeDests(numDests, dests);
    return 0;
}


typedef struct CupsConnection {
    cups_dest_t * dests, * dest;
    int numDests;
    cups_dinfo_t * dinfo;
    http_t * http;
} CupsConnection;


static CupsConnection * openCups(const char * printer) {
    CupsConnection * cups = (CupsConnection *)g_malloc0(sizeof(CupsConnection));
    char * name = g_strdup(printer), * instance;
    if ((instance = g_strrstr(name, "/")) != NULL)
        *instance++ = '\0';
    cups->numDests = cupsGetDests(&cups->dests);
    if (cups->dests) {
        cups->dest = cupsGetDest(name, instance, cups->numDests, cups->dests);
        if (cups->dest) {
            cups->http = cupsConnectDest(cups->dest, CUPS_DEST_FLAGS_NONE,
                                         30000, NULL, NULL, 0, NULL, NULL);
            if (cups->http) {
                cups->dinfo = cupsCopyDestInfo(cups->http, cups->dest);
            }
        }
    }
    g_free(name);
    return cups;
}


static void closeCups(CupsConnection * cups) {
    cupsFreeDestInfo(cups->dinfo);
    httpClose(cups->http);
    cupsFreeDests(cups->numDests, cups->dests);
    g_free(cups);
}


static ipp_attribute_t * ippIsSupported(CupsConnection * cups, const char * attrName) {
    return cupsFindDestSupported(cups->http, cups->dest, cups->dinfo, attrName);
}


static ipp_attribute_t * ippGetDefault(CupsConnection * cups, const char * attrName) {
    return cupsFindDestDefault(cups->http, cups->dest, cups->dinfo, attrName);
}


static int ippHasOtherThan(CupsConnection * cups, const char * attrName, const char * value) {
    ipp_attribute_t * attr = ippIsSupported(cups, attrName);
    if (attr) {
        int i = ippGetCount(attr) - 1;
        while (i >= 0 && !g_ascii_strcasecmp(ippGetString(attr, i, NULL), value)) --i;
        return i >= 0;
    }
    return FALSE;
}


static void getResolutions(PPDGenerator * ppd, CupsConnection * cups) {
    ipp_attribute_t * attr;
    if (attr = ippIsSupported(cups, "printer-resolution")) {
        int i = ippGetCount(attr) - 1, yres;
        ipp_res_t units;
        while (i >= 0) {
            ppdAddResolution(ppd, ippGetResolution(attr, i--, &yres, &units));
        }
        if (attr = ippGetDefault(cups, "printer-resolution")) {
            ppdSetDefaultResolution(ppd, ippGetResolution(attr, 0, &yres, &units));
        }
    }
}


static char * getPrettyName(const char * pwg) {
    static char name[1024];
    // Get middle component
    const char * start = strchr(pwg, '_');
    if (start) {
        const char * end = strchr(++start, '_');
        g_strlcpy(name, start, end ? end - start + 1 : 1024);
    } else {
        g_strlcpy(name, pwg, 1024);
    }
    // Turn _ into spaces
    g_strdelimit(name, "_", ' ');
    // For each '-', remove it and capitalize next letter
    int capitalize = TRUE;
    char * i, * j;
    for (i = name, j = i; *i != '\0'; ++i) {
        if (*i != '-')
            *j++ = capitalize ? g_ascii_toupper(*i) : *i;
        capitalize = *i == '-';
    }
    *j = '\0';
    return name;
}


static void getPapers(PPDGenerator * ppd, CupsConnection * cups) {
    ipp_attribute_t * attr = ippIsSupported(cups, "media");
    if (attr) {
        int i = ippGetCount(attr) - 1;
        while (i >= 0) {
            pwg_media_t * size = pwgMediaForPWG(ippGetString(attr, i--, NULL));
            if (g_str_has_prefix(size->pwg, "custom")) continue;
            ppdAddPaperSize(ppd, size->ppd ? size->ppd : getPrettyName(size->pwg),
                            size->width * 72 / 2540, size->length * 72 / 2540);
        }
        if (attr = ippGetDefault(cups, "media")) {
            pwg_media_t * size = pwgMediaForPWG(ippGetString(attr, 0, NULL));
            ppdSetDefaultPaperSize(ppd, size->ppd ? size->ppd : getPrettyName(size->pwg));
        }
    }
    // TODO: get media margins
}


const char * capitalizeFirst(const char * str) {
    static char buffer[100];
    g_strlcpy(buffer, str, 100);
    buffer[0] = g_ascii_toupper(buffer[0]);
    return buffer;
}


static void getMediaSources(PPDGenerator * ppd, CupsConnection * cups) {
    ipp_attribute_t * attr = ippIsSupported(cups, CUPS_MEDIA_SOURCE);
    if (attr) {
        int i, count = ippGetCount(attr);
        for (i = 0; i < count; ++i) {
            ppdAddTray(ppd, capitalizeFirst(ippGetString(attr, i, NULL)));
        }
        if (attr = ippGetDefault(cups, CUPS_MEDIA_SOURCE)) {
            ppdSetDefaultTray(ppd, capitalizeFirst(ippGetString(attr, 0, NULL)));
        }
    }
}


static void getMediaTypes(PPDGenerator * ppd, CupsConnection * cups) {
    ipp_attribute_t * attr = ippIsSupported(cups, CUPS_MEDIA_TYPE);
    if (attr) {
        int i, count = ippGetCount(attr);
        for (i = 0; i < count; ++i) {
            ppdAddMediaType(ppd, capitalizeFirst(ippGetString(attr, i, NULL)));
        }
        if (attr = ippGetDefault(cups, CUPS_MEDIA_TYPE)) {
            ppdSetDefaultMediaType(ppd, capitalizeFirst(ippGetString(attr, 0, NULL)));
        }
    }
}


char * getPPDFile(const char * printer) {
    char * result = NULL;
    int i;
    PPDGenerator * ppd = newPPDGenerator(printer);
    CupsConnection * cups = openCups(printer);

    if (cups->dinfo) {
        ppdSetColor(ppd, ippHasOtherThan(cups, CUPS_PRINT_COLOR_MODE, "monochrome"));
        ppdSetDuplex(ppd, ippHasOtherThan(cups, CUPS_SIDES, "one-sided"));
        getResolutions(ppd, cups);
        getPapers(ppd, cups);
        getMediaSources(ppd, cups);
        getMediaTypes(ppd, cups);
        result = g_strdup(generatePPD(ppd));
    }

    closeCups(cups);
    deletePPDGenerator(ppd);
    return result;
}


void printJob(PrintJob * job) {
    openWithApp(job->name);
}