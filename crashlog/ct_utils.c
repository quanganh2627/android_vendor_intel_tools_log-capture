/* Copyright (C) Intel 2013
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file ct_utils.c
 * @brief File containing basic operations for crashlog to kernel and
 * crashlog to user space communication
 */

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <ctype.h>
#include <resolv.h>
#include "cutils/properties.h"
#include "privconfig.h"
#include "crashutils.h"
#include "fsutils.h"
#include "ct_utils.h"

#define BINARY_SUFFIX  ".bin"
#define PROP_PREFIX    "dev.log"

static const char *suffixes[] = {
    [CT_EV_STAT]    = "_trigger",
    [CT_EV_INFO]    = "_infoevent",
    [CT_EV_ERROR]   = "_errorevent",
    [CT_EV_CRASH]   = "_errorevent",
    [CT_EV_LAST]    = "_ignored"
};

void handle_event(struct ct_event *ev) {

    char submitter[PROPERTY_KEY_MAX];
    char propval[PROPERTY_VALUE_MAX];

    if (ev->type >= CT_EV_LAST) {
        LOGE("Unknown event type '%d', discarding", ev->type);
        return;
    }

    snprintf(submitter, sizeof(submitter), "%s.%s",
            PROP_PREFIX, ev->submitter_name);

    /*
     * to reduce confusion:
     * property can be either ON/OFF for a given submitter.
     * if it's ON, we want event not to be filtered
     * if it's OFF, we want event to be filtered
     * event should be flagged to be manage by this property.
     */
    if (ev->flags & EV_FLAGS_PRIORITY_LOW) {
        if (!property_get(submitter, propval, NULL))
            return;
        if (strcmp(propval, "ON"))
            return;
    }
}

void process_msg(struct ct_event *ev)
{
    char destination[PATHMAX];
    char name[MAX_SB_N+MAX_EV_N+2];
    char name_event[20];
    e_dir_mode_t mode;
    char *dir_mode;
    int dir;
    char *key;

    /* Temporary implementation: Crashlog handles Kernel CRASH events
     * as if they were Kernel ERROR events
     */
    switch (ev->type) {
    case CT_EV_STAT:
        mode = MODE_STATS;
        dir_mode = STATS_DIR;
        snprintf(name_event, sizeof(name_event), "%s", STATSEVENT);
        break;
    case CT_EV_INFO:
        mode = MODE_STATS;
        dir_mode = STATS_DIR;
        LOGI("%s: Event CT_EV_INFO", __FUNCTION__);
        snprintf(name_event, sizeof(name_event), "%s", INFOEVENT);
        LOGI("%s: Event CT_EV_INFO %s", __FUNCTION__, name_event);
        break;
    case CT_EV_ERROR:
    case CT_EV_CRASH:
        mode = MODE_STATS;
        LOGI("%s: Event CT_EV_CRASH", __FUNCTION__);
        dir_mode = STATS_DIR;
        snprintf(name_event, sizeof(name_event), "%s", ERROREVENT);
        break;
    case CT_EV_LAST:
    default:
        LOGE("%s: unknown event type\n", __FUNCTION__);
        return;
    }

    /* Compute name */
    snprintf(name, sizeof(name), "%s_%s", ev->submitter_name, ev->ev_name);
    /* Convert lower-case name into upper-case name */
    convert_name_to_upper_case(name);

    dir = find_new_crashlog_dir(mode);
    if (dir < 0) {
        LOGE("%s: Cannot get a valid new crash directory...\n", __FUNCTION__);
        key = raise_event(name_event, name, NULL, NULL);
        LOGE("%-8s%-22s%-20s%s\n", name_event, key,
                get_current_time_long(0), name);
        free(key);
        return;
    }

    if (ev->attchmt_size) {
        /* copy binary data into file */
        snprintf(destination, sizeof(destination), "%s%d/%s_%s%s",
                dir_mode, dir,
                ev->submitter_name, ev->ev_name, BINARY_SUFFIX);

        dump_binary_attchmts_in_file(ev, destination);
    }

    snprintf(destination, sizeof(destination), "%s%d/%s_%s%s",
            dir_mode, dir,
            ev->submitter_name, ev->ev_name, suffixes[ev->type]);

    /*
     * Here we copy only DATA{0,1,2} in the trig file, because crashtool
     * does not understand any other types. We attach others types in the
     * data file thanks to the function dump_binary_attchmts_in_file();
     */

    dump_data_in_file(ev, destination);

    snprintf(destination, sizeof(destination), "%s%d/", dir_mode, dir);
    key = raise_event(name_event, name, NULL, destination);
    LOGE("%-8s%-22s%-20s%s %s\n", name_event, key,
            get_current_time_long(0), name, destination);
    free(key);
}

int dump_binary_attchmts_in_file(struct ct_event* ev, char* file_path) {

    struct ct_attchmt* at = NULL;
    char *b64encoded = NULL;
    FILE *file = NULL;
    int nr_binary = 0;

    LOGI("Creating %s\n", file_path);

    file = fopen(file_path, "w+");
    if (!file) {
        LOGE("can't open '%s' : %s\n", file_path, strerror(errno));
        return -1;
    }

    foreach_attchmt(ev, at) {
        switch (at->type) {
        case CT_ATTCHMT_BINARY:
            b64encoded = calloc(1, (at->size+2)*4/3);
            b64_ntop((u_char*)at->data, at->size,
                    b64encoded, (at->size+2)*4/3);
            fprintf(file, "BINARY%d=%s\n", nr_binary, b64encoded);
            ++nr_binary;
            free(b64encoded);
            break;
        case CT_ATTCHMT_DATA0:
        case CT_ATTCHMT_DATA1:
        case CT_ATTCHMT_DATA2:
    case CT_ATTCHMT_DATA3:
    case CT_ATTCHMT_DATA4:
    case CT_ATTCHMT_DATA5:
        /* Nothing to do */
            break;
        default:
            LOGE("Ignoring unknown attachment type: %d\n", at->type);
            break;
        }
    }

    fclose(file);

    /* No binary data in attachment. File shall be removed */
    if (!nr_binary)
        remove(file_path);

    return 0;
}

int dump_data_in_file(struct ct_event* ev, char* file_path) {

    struct ct_attchmt* att = NULL;
    FILE *file = NULL;

    LOGI("Creating %s\n", file_path);

    file = fopen(file_path, "w+");
    if (!file) {
        LOGE("can't open '%s' : %s\n", file_path, strerror(errno));
        return -1;
    }

    foreach_attchmt(ev, att) {
        switch (att->type) {
        case CT_ATTCHMT_DATA0:
            fprintf(file, "DATA0=%s\n", att->data);
            break;
        case CT_ATTCHMT_DATA1:
            fprintf(file, "DATA1=%s\n", att->data);
            break;
        case CT_ATTCHMT_DATA2:
            fprintf(file, "DATA2=%s\n", att->data);
            break;
        case CT_ATTCHMT_DATA3:
            fprintf(file, "DATA3=%s\n", att->data);
            break;
        case CT_ATTCHMT_DATA4:
            fprintf(file, "DATA4=%s\n", att->data);
            break;
        case CT_ATTCHMT_DATA5:
            fprintf(file, "DATA5=%s\n", att->data);
            break;
        default:
            break;
        }
    }

    fclose(file);

    return 0;
}

void convert_name_to_upper_case(char * name) {

    unsigned int i;

    for (i=0; i<strlen(name); i++) {
        name[i] = toupper(name[i]);
    }
}

