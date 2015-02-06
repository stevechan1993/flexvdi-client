/**
 * Copyright Flexible Software Solutions S.L. 2014
 **/

#include <stdio.h>
#include <stdlib.h>
#include <glib.h>
#include <unistd.h>
#include "printclient.h"


typedef struct PrintJob {
    gint fileHandle;
    gchar * name;
} PrintJob;

static GHashTable * printJobs;


void initPrintClient() {
    printJobs = g_hash_table_new_full(g_direct_hash, NULL, NULL, g_free);
}


static void openWithApp(const char * file) {
    char command[1024];
#ifdef WIN32
    snprintf(command, 1024, "start %s", file);
#else
    snprintf(command, 1024, "xdg-open %s", file);
    // TODO: on Mac OS X, the command is 'open'
#endif
    system(command);
}


void handlePrintJob(FlexVDIPrintJobMsg * msg) {
    PrintJob * job = g_malloc(sizeof(PrintJob));
    job->fileHandle = g_file_open_tmp("fpjXXXXXX.pdf", &job->name, NULL);
    g_hash_table_insert(printJobs, GINT_TO_POINTER(msg->id), job);
}


void handlePrintJobData(FlexVDIPrintJobDataMsg * msg) {
    PrintJob * job = g_hash_table_lookup(printJobs, GINT_TO_POINTER(msg->id));
    if (job) {
        if (!msg->dataLength) {
            close(job->fileHandle);
            openWithApp(job->name);
            g_free(job->name);
            g_hash_table_remove(printJobs, GINT_TO_POINTER(msg->id));
            // TODO: Remove the temporary file
        } else {
            write(job->fileHandle, msg->data, msg->dataLength);
        }
    } else {
        printf("Job %d not found\n", msg->id);
    }
}
