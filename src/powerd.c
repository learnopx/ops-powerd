/*
 * (c) Copyright 2015 Hewlett Packard Enterprise Development LP
 * Copyright (c) 2008, 2009, 2010, 2011, 2012, 2013, 2014 Nicira, Inc.
 * All Rights Reserved.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License"); you may
 *   not use this file except in compliance with the License. You may obtain
 *   a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 *   WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 *   License for the specific language governing permissions and limitations
 *   under the License.
 */

/************************************************************************//**
 * @ingroup ops-powerd
 *
 * @file
 * Source file for the platform Power daemon
 ***************************************************************************/

#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "command-line.h"
#include "compiler.h"
#include "daemon.h"
#include "dirs.h"
#include "dummy.h"
#include "fatal-signal.h"
#include "ovsdb-idl.h"
#include "poll-loop.h"
#include "simap.h"
#include "stream-ssl.h"
#include "stream.h"
#include "svec.h"
#include "timeval.h"
#include "unixctl.h"
#include "util.h"
#include "openvswitch/vconn.h"
#include "openvswitch/vlog.h"
#include "vswitch-idl.h"
#include "coverage.h"
#include "config-yaml.h"
#include "powerd.h"
#include "eventlog.h"

static struct ovsdb_idl *idl;

static unsigned int idl_seqno;

static unixctl_cb_func powerd_unixctl_dump;

static bool cur_hw_set = false;

YamlConfigHandle yaml_handle;

struct shash psu_data;       /* struct locl_psu (all psus) */
struct shash subsystem_data; /* struct locl_subsystem */

/* map psustatus enum to the equivalent string */
static const char *
psu_status_to_string(enum psustatus status)
{
    VLOG_DBG("psu status is %d", status);
    if ((unsigned int)status < sizeof(psu_status)/sizeof(const char *)) {
        VLOG_DBG("psu status is %s", psu_status[status]);
        return(psu_status[status]);
    } else {
        VLOG_DBG("psu status is %s", psu_status[PSU_STATUS_OK]);
        return(psu_status[PSU_STATUS_OK]);
    }
}

static enum psustatus
psu_string_to_status(const char *string)
{
    unsigned int idx;

    if (strcmp(string, "none") == 0) {
        return(PSU_STATUS_OVERRIDE_NONE);
    }

    for (idx = 0; idx < sizeof(psu_status)/sizeof(const char *); idx++) {
        if (strcmp(string, psu_status[idx]) == 0) {
            return(idx);
        }
    }

    return(PSU_STATUS_UNKNOWN);
}

/* initialize the subsystem and global psu dictionaries */
static void
init_subsystems(void)
{
    shash_init(&subsystem_data);
    shash_init(&psu_data);
}

/* find a psu (in idl cache) by name
   used for mapping existing db object to yaml object */
static struct ovsrec_power_supply *
lookup_psu(const char *name)
{
    const struct ovsrec_power_supply *psu;

    OVSREC_POWER_SUPPLY_FOR_EACH(psu, idl) {
        if (strcmp(psu->name, name) == 0) {
            return((struct ovsrec_power_supply *)psu);
        }
    }

    return(NULL);
}

static enum bit_op_result
get_bool_op(const char *subsystem_name, const char *psu_name, const i2c_bit_op *psu_op)
{
    uint32_t value;
    int rc;

    rc = i2c_reg_read(yaml_handle, subsystem_name, psu_op, &value);

    if (rc != 0) {
        VLOG_WARN("subsystem %s: unable to read byte for psu %s status (%d)",
            subsystem_name, psu_name, rc);
        return(BIT_OP_FAIL);
    }

    return (value == psu_op->bit_mask) ? BIT_OP_STATUS_OK : BIT_OP_STATUS_BAD;
}

static void
powerd_read_psu(struct locl_psu *psu)
{
    const YamlPsu *yaml_psu = psu->yaml_psu;
    enum bit_op_result present, input_ok, output_ok;

    VLOG_DBG("reading psu %s state", psu->name);
    /* read presence, input, and output */
    present = get_bool_op(psu->subsystem->name, psu->name, yaml_psu->psu_present);

    input_ok = get_bool_op(psu->subsystem->name, psu->name, yaml_psu->psu_input_ok);

    output_ok = get_bool_op(psu->subsystem->name, psu->name, yaml_psu->psu_output_ok);

    if (present == BIT_OP_STATUS_BAD) {
        psu->status = PSU_STATUS_FAULT_ABSENT;
    } else if (input_ok == BIT_OP_STATUS_BAD) {
        psu->status = PSU_STATUS_FAULT_INPUT;
    } else if (output_ok == BIT_OP_STATUS_BAD) {
        psu->status = PSU_STATUS_FAULT_OUTPUT;
    } else {
        psu->status = PSU_STATUS_OK;
    }

    if (present == BIT_OP_FAIL ||
        input_ok == BIT_OP_FAIL ||
        output_ok == BIT_OP_FAIL) {
        psu->status = PSU_STATUS_UNKNOWN;
    }

    if (psu->test_status != PSU_STATUS_OVERRIDE_NONE) {
        psu->status = psu->test_status;
    }
}

static void
powerd_set_psuleds(struct locl_subsystem *subsystem)
{
    const YamlPsuInfo *psu_info;
    struct locl_psu *psu;
    struct shash_node *psu_node;
    enum psustatus status = PSU_STATUS_OK;
    unsigned char ledval ;
    int rc = 0;

    psu_info = yaml_get_psu_info(yaml_handle, subsystem->name);
    if (psu_info == NULL) {
        VLOG_DBG("subsystem %s has no psu info", subsystem->name);
        return;
    }
    if (psu_info->psu_led == NULL)
        return;

    SHASH_FOR_EACH(psu_node, &subsystem->subsystem_psus) {
        psu = (struct locl_psu *)psu_node->data;
        switch(psu->status) {
        case PSU_STATUS_OK:
        case PSU_STATUS_UNKNOWN:
        case PSU_STATUS_OVERRIDE_NONE:
        case PSU_STATUS_FAULT_ABSENT:
            /* Ignore absent PSUs and OK status */
            break;
        case PSU_STATUS_FAULT_INPUT:
        case PSU_STATUS_FAULT_OUTPUT:
            status = psu->status;
            break;
        }
    }

    if (subsystem->status != status) {
        subsystem->status = status;
        ledval = psu_info->psu_led_values.off;
        switch(status) {
        case PSU_STATUS_OK:
            ledval = psu_info->psu_led_values.good;
            break;
        case PSU_STATUS_FAULT_INPUT:
        case PSU_STATUS_FAULT_OUTPUT:
        case PSU_STATUS_FAULT_ABSENT:
            ledval = psu_info->psu_led_values.fault;
            break;
        case PSU_STATUS_UNKNOWN:
        case PSU_STATUS_OVERRIDE_NONE:
            ledval = psu_info->psu_led_values.off;
            break;
        }

        rc = i2c_reg_write(yaml_handle, subsystem->name,
                           psu_info->psu_led, ledval);
        if (rc) {
            VLOG_DBG("Unable to set subsystem %s psu status LED",
                     subsystem->name);
        }
    }
}


/************************************************************************//**
 * Function that creates a new locl_subsystem structure when a new
 *    subsystem is found in ovsdb, reads the psu status for each power
 *    supply, and adds the power supplies into the ovsdb Power_supply table.
 *
 * Logic:
 *      - create a new locl_subsystem structure, add to hash
 *      - tag the subsystem as "unmarked" and as IGNORE
 *      - extract the psu information for this subsys from the hw desc files.
 *      - foreach valid power supply
 *          - read psu status
 *          - add the psu to the Power_supply table (add to transaction)
 *      - tag the subsystem as "marked" and as OK
 *      - commit the transaction
 *
 * Returns:  struct locl_subsystem * on success, else NULL on failure
 ***************************************************************************/
static struct locl_subsystem *
add_subsystem(const struct ovsrec_subsystem *ovsrec_subsys)
{
    struct locl_subsystem *result;
    int rc;
    int idx;
    struct ovsdb_idl_txn *txn;
    struct ovsrec_power_supply **psu_array;
    int psu_idx;
    int psu_count;
    const char *dir;

    /* get the hw_desc_dir location */
    dir = ovsrec_subsys->hw_desc_dir;

    if (dir == NULL || strlen(dir) == 0) {
        VLOG_WARN("No hardware description file directory for subsystem %s", ovsrec_subsys->name);
        return(NULL);
    }

    /* create and initialize basic subsystem information */
    VLOG_DBG("Adding new subsystem %s", ovsrec_subsys->name);
    result = (struct locl_subsystem *)malloc(sizeof(struct locl_subsystem));
    memset(result, 0, sizeof(struct locl_subsystem));
    (void)shash_add(&subsystem_data, ovsrec_subsys->name, (void *)result);
    result->name = strdup(ovsrec_subsys->name);
    result->marked = false;
    result->valid = false;
    result->status = PSU_STATUS_UNKNOWN;
    result->parent_subsystem = NULL;  /* OPS_TODO: find parent subsystem */
    shash_init(&result->subsystem_psus);

    /* since this is a new subsystem, load all of the hardware description
       information about devices and psus (just for this subsystem).
       parse psus and device data for subsystem */
    rc = yaml_add_subsystem(yaml_handle, ovsrec_subsys->name, dir);

    if (rc != 0) {
        VLOG_ERR("Error reading h/w desc files for subsystem %s",
                 ovsrec_subsys->name);
        return(NULL);
    }

    /* need devices data */
    rc = yaml_parse_devices(yaml_handle, ovsrec_subsys->name);

    if (rc != 0) {
        VLOG_ERR("Unable to parse subsystem %s devices file (in %s)",
                 ovsrec_subsys->name, dir);
        return(NULL);
    }

    /* need psu data */
    rc = yaml_parse_psus(yaml_handle, ovsrec_subsys->name);

    if (rc != 0) {
        VLOG_ERR("Unable to parse subsystem %s power file (in %s)",
                 ovsrec_subsys->name, dir);
        return(NULL);
    }

    /* OPS_TODO: The thermal info has a polling period, but when we
       OPS_TODO: have multiple subsystems, that could be tricky to
       OPS_TODO: implement if there are different polling periods.
       OPS_TODO: For now, hardwire the polling period to 5 seconds. */

    /* prepare to add psus to db */
    psu_idx = 0;
    psu_count = yaml_get_psu_count(yaml_handle, ovsrec_subsys->name);

    if (psu_count <= 0) {
        return(NULL);
    }

    result->valid = true;

    /* subsystem db object has reference array for psus */
    psu_array = (struct ovsrec_power_supply **)malloc(psu_count * sizeof(struct ovsrec_power_supply *));
    memset(psu_array, 0, psu_count * sizeof(struct ovsrec_power_supply *));

    txn = ovsdb_idl_txn_create(idl);

    VLOG_DBG("There are %d psus in subsystem %s", psu_count, ovsrec_subsys->name);
    log_event("POWER_COUNT", EV_KV("count", "%d", psu_count),
        EV_KV("subsystem", "%s", ovsrec_subsys->name));

    for (idx = 0; idx < psu_count; idx++) {
        const YamlPsu *psu = yaml_get_psu(yaml_handle, ovsrec_subsys->name, idx);

        struct ovsrec_power_supply *ovs_psu;
        char *psu_name = NULL;
        struct locl_psu *new_psu;
        VLOG_DBG("Adding psu %d in subsystem %s",
            psu->number,
            ovsrec_subsys->name);

        /* create a name for the psu from the subsystem name and the
           psu number */
        asprintf(&psu_name, "%s-%d", ovsrec_subsys->name, psu->number);
        /* allocate and initialize basic psu information */
        new_psu = (struct locl_psu *)malloc(sizeof(struct locl_psu));
        new_psu->name = psu_name;
        new_psu->subsystem = result;
        new_psu->yaml_psu = psu;
        new_psu->status = PSU_STATUS_OK;
        /* no test override set */
        new_psu->test_status = PSU_STATUS_OVERRIDE_NONE;

        /* try to populate psu status with real data */
        powerd_read_psu(new_psu);

        /* add psu to subsystem psu dictionary */
        shash_add(&result->subsystem_psus, psu_name, (void *)new_psu);
        /* add psu to global psu dictionary */
        shash_add(&psu_data, psu_name, (void *)new_psu);

        /* look for existing Power_supply rows */
        ovs_psu = lookup_psu(psu_name);

        if (ovs_psu == NULL) {
            /* existing psu doesn't exist in db, create it */
            ovs_psu = ovsrec_power_supply_insert(txn);
        }

        /* set initial data */
        ovsrec_power_supply_set_name(ovs_psu, psu_name);
        ovsrec_power_supply_set_status(ovs_psu,
            psu_status_to_string(new_psu->status));

        /* add psu to subsystem reference list */
        psu_array[psu_idx++] = ovs_psu;
    }

    ovsrec_subsystem_set_power_supplies(ovsrec_subsys, psu_array, psu_count);
    /* execute transaction */
    ovsdb_idl_txn_commit_block(txn);
    ovsdb_idl_txn_destroy(txn);
    free(psu_array);

    return(result);
}

static void
powerd_unixctl_test(struct unixctl_conn *conn, int argc OVS_UNUSED,
                    const char *argv[], void *aud OVS_UNUSED)
{
    struct locl_psu *psu;
    const char *psu_name = argv[1];
    struct shash_node *node;
    enum psustatus state;

    state = psu_string_to_status(argv[2]);

    /* find the psu structure */
    node = shash_find(&psu_data, psu_name);
    if (node == NULL) {
        unixctl_command_reply_error(conn, "Power supply does not exist");
        return;
    }
    psu = (struct locl_psu *)node->data;

    /* set the override value */
    psu->test_status = state;
    unixctl_command_reply(conn, "Test power status override set");
}

/* initialize powerd process */
static void
powerd_init(const char *remote)
{
    int retval;

    /* initialize subsystems */
    init_subsystems();

    /* initialize the yaml handle */
    yaml_handle = yaml_new_config_handle();

    /* create connection to db */
    idl = ovsdb_idl_create(remote, &ovsrec_idl_class, false, true);
    idl_seqno = ovsdb_idl_get_seqno(idl);
    ovsdb_idl_set_lock(idl, "ops_powerd");
    ovsdb_idl_verify_write_only(idl);

    /* Register for daemon table. */
    ovsdb_idl_add_table(idl, &ovsrec_table_daemon);
    ovsdb_idl_add_column(idl, &ovsrec_daemon_col_name);
    ovsdb_idl_add_column(idl, &ovsrec_daemon_col_cur_hw);
    ovsdb_idl_omit_alert(idl, &ovsrec_daemon_col_cur_hw);

    ovsdb_idl_add_table(idl, &ovsrec_table_power_supply);
    ovsdb_idl_add_column(idl, &ovsrec_power_supply_col_status);
    ovsdb_idl_omit_alert(idl, &ovsrec_power_supply_col_status);
    ovsdb_idl_add_column(idl, &ovsrec_power_supply_col_name);
    ovsdb_idl_omit_alert(idl, &ovsrec_power_supply_col_name);

    ovsdb_idl_add_table(idl, &ovsrec_table_subsystem);
    ovsdb_idl_add_column(idl, &ovsrec_subsystem_col_name);
    ovsdb_idl_add_column(idl, &ovsrec_subsystem_col_power_supplies);
    ovsdb_idl_omit_alert(idl, &ovsrec_subsystem_col_power_supplies);
    ovsdb_idl_add_column(idl, &ovsrec_subsystem_col_hw_desc_dir);
    ovsdb_idl_omit_alert(idl, &ovsrec_subsystem_col_hw_desc_dir);

    unixctl_command_register("ops-powerd/dump", "", 0, 0,
                             powerd_unixctl_dump, NULL);
    unixctl_command_register("ops-powerd/test", "psu state", 2, 2,
                             powerd_unixctl_test, NULL);

    retval = event_log_init("POWER");
    if(retval < 0) {
        VLOG_ERR("Event log initialization failed for POWER");
    }
}

/* pre-exit shutdown processing */
static void
powerd_exit(void)
{
    ovsdb_idl_destroy(idl);
}

/* poll every psu for new state */
static void
powerd_run__(void)
{
    struct ovsdb_idl_txn *txn;
    const struct ovsrec_power_supply *cfg;
    const struct ovsrec_daemon *db_daemon;
    struct shash_node *node;
    struct shash_node *psu_node;
    struct locl_psu *psu;
    bool change = false;

    SHASH_FOR_EACH(node, &subsystem_data) {
        struct locl_subsystem *subsystem = (struct locl_subsystem *)node->data;
        if (!subsystem->valid) {
            continue;
        }
        SHASH_FOR_EACH(psu_node, &subsystem->subsystem_psus) {
            psu = (struct locl_psu *)psu_node->data;
            powerd_read_psu(psu);
        }
    }

    txn = ovsdb_idl_txn_create(idl);
    OVSREC_POWER_SUPPLY_FOR_EACH(cfg, idl) {
        const char *status;
        node = shash_find(&psu_data, cfg->name);
        if (node == NULL) {
            VLOG_WARN("unable to find matching psu for %s", cfg->name);
            ovsrec_power_supply_set_status(
                cfg,
                psu_status_to_string(PSU_STATUS_OK));
            change = true;
            continue;
        }
        psu = (struct locl_psu *)node->data;

        /* note: only apply changes - don't blindly set data */

        /* calculate and set status */
        status = psu_status_to_string(psu->status);
        if (strcmp(status, cfg->status) != 0) {
            ovsrec_power_supply_set_status(cfg, status);
            change = true;
        }
    }

    /* If first time through, set cur_hw = 1 */
    if (!cur_hw_set) {
        OVSREC_DAEMON_FOR_EACH(db_daemon, idl) {
            if (strncmp(db_daemon->name, NAME_IN_DAEMON_TABLE,
                        strlen(NAME_IN_DAEMON_TABLE)) == 0) {
                ovsrec_daemon_set_cur_hw(db_daemon, (int64_t) 1);
                cur_hw_set = true;
                change = true;
                break;
            }
        }
    }

    /* if a change was made, execute the transaction */
    if (change == true) {
        ovsdb_idl_txn_commit_block(txn);
    }
    ovsdb_idl_txn_destroy(txn);
}

/* lookup a local subsystem structure */
/* if it's not found, create a new one and initialize it */
static struct locl_subsystem *
get_subsystem(const struct ovsrec_subsystem *ovsrec_subsys)
{
    void *ptr;
    struct locl_subsystem *result = NULL;

    ptr = shash_find_data(&subsystem_data, ovsrec_subsys->name);

    if (ptr == NULL) {
        /* this subsystem has not been added, yet. Do that now. */
        result = add_subsystem(ovsrec_subsys);
    } else {
        result = (struct locl_subsystem *)ptr;
        if (!result->valid) {
            result = NULL;
        }
    }

    return(result);
}

/* set the "marked" value for each subsystem to false. */
static void
powerd_unmark_subsystems(void)
{
    struct shash_node *node;

    SHASH_FOR_EACH(node, &subsystem_data) {
        struct locl_subsystem *subsystem = (struct locl_subsystem *)node->data;
        subsystem->marked = false;
    }
}

/************************************************************************//**
 * Function that will remove the internal entry in the locl_subsystem hash
 * for any subsystem that is no longer in OVSDB.
 *
 * @todo OPS_TODO: need to remove subsystem yaml data
 ***************************************************************************/
static void
powerd_remove_unmarked_subsystems(void)
{
    struct shash_node *node, *next;
    struct shash_node *temp_node, *temp_next;
    struct shash_node *global_node;

    SHASH_FOR_EACH_SAFE(node, next, &subsystem_data) {
        struct locl_subsystem *subsystem = node->data;

        if (subsystem->marked == false) {
            /* also, delete all psus in the subsystem */
            SHASH_FOR_EACH_SAFE(temp_node, temp_next, &subsystem->subsystem_psus) {
                struct locl_psu *temp = (struct locl_psu *)temp_node->data;
                /* delete the psu_data entry */
                global_node = shash_find(&psu_data, temp->name);
                shash_delete(&psu_data, global_node);
                /* delete the subsystem entry */
                shash_delete(&subsystem->subsystem_psus, temp_node);
                /* free the allocated data */
                free(temp->name);
                free(temp);
            }
            free(subsystem->name);
            free(subsystem);

            /* delete the subsystem dictionary entry */
            shash_delete(&subsystem_data, node);

            /* OPS_TODO: need to remove subsystem yaml data */
        }
    }
}

/* process any changes to cached data */
static void
powerd_reconfigure(struct ovsdb_idl *idl)
{
    const struct ovsrec_subsystem *subsys;
    unsigned int new_idl_seqno = ovsdb_idl_get_seqno(idl);

    COVERAGE_INC(powerd_reconfigure);

    if (new_idl_seqno == idl_seqno) {
        return;
    }

    idl_seqno = new_idl_seqno;

    /* handle any added or deleted subsystems */
    powerd_unmark_subsystems();

    OVSREC_SUBSYSTEM_FOR_EACH(subsys, idl) {
        struct locl_subsystem *subsystem;
        /* get_subsystem will create a new one if it was added */
        subsystem = get_subsystem(subsys);
        if (subsystem == NULL) {
            continue;
        }
        powerd_set_psuleds(subsystem);
        subsystem->marked = true;
    }

    /* remove any subsystems that are no longer present in the db */
    powerd_remove_unmarked_subsystems();
}

/* perform all of the per-loop processing */
static void
powerd_run(void)
{
    ovsdb_idl_run(idl);

    if (ovsdb_idl_is_lock_contended(idl)) {
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 1);

        VLOG_ERR_RL(&rl, "another ops-powerd process is running, "
                    "disabling this process until it goes away");

        return;
    } else if (!ovsdb_idl_has_lock(idl)) {
        return;
    }

    /* handle changes to cache */
    powerd_reconfigure(idl);
    /* poll all psus and report changes into db */
    powerd_run__();

    daemonize_complete();
    vlog_enable_async();
    VLOG_INFO_ONCE("%s (OpenSwitch powerd) %s", program_name, VERSION);
}

/* initialize periodic poll of psus */
static void
powerd_wait(void)
{
    ovsdb_idl_wait(idl);
    poll_timer_wait(POLLING_PERIOD * MSEC_PER_SEC);
}

static void
powerd_unixctl_dump(struct unixctl_conn *conn, int argc OVS_UNUSED,
                          const char *argv[] OVS_UNUSED, void *aux OVS_UNUSED)
{
    unixctl_command_reply_error(conn, "Nothing to dump :)");
}


static unixctl_cb_func ops_powerd_exit;

static char *parse_options(int argc, char *argv[], char **unixctl_path);
OVS_NO_RETURN static void usage(void);

int
main(int argc, char *argv[])
{
    char *unixctl_path = NULL;
    struct unixctl_server *unixctl;
    char *remote;
    bool exiting;
    int retval;

    set_program_name(argv[0]);

    proctitle_init(argc, argv);
    remote = parse_options(argc, argv, &unixctl_path);
    fatal_ignore_sigpipe();

    ovsrec_init();

    daemonize_start();

    retval = unixctl_server_create(unixctl_path, &unixctl);
    if (retval) {
        exit(EXIT_FAILURE);
    }
    unixctl_command_register("exit", "", 0, 0, ops_powerd_exit, &exiting);

    powerd_init(remote);
    free(remote);

    exiting = false;
    while (!exiting) {
        powerd_run();
        unixctl_server_run(unixctl);

        powerd_wait();
        unixctl_server_wait(unixctl);
        if (exiting) {
            poll_immediate_wake();
        }
        poll_block();
    }
    powerd_exit();
    unixctl_server_destroy(unixctl);

    return 0;
}

static char *
parse_options(int argc, char *argv[], char **unixctl_pathp)
{
    enum {
        OPT_PEER_CA_CERT = UCHAR_MAX + 1,
        OPT_UNIXCTL,
        VLOG_OPTION_ENUMS,
        OPT_BOOTSTRAP_CA_CERT,
        OPT_ENABLE_DUMMY,
        OPT_DISABLE_SYSTEM,
        DAEMON_OPTION_ENUMS,
        OPT_DPDK,
    };
    static const struct option long_options[] = {
        {"help",        no_argument, NULL, 'h'},
        {"version",     no_argument, NULL, 'V'},
        {"unixctl",     required_argument, NULL, OPT_UNIXCTL},
        DAEMON_LONG_OPTIONS,
        VLOG_LONG_OPTIONS,
        STREAM_SSL_LONG_OPTIONS,
        {"peer-ca-cert", required_argument, NULL, OPT_PEER_CA_CERT},
        {"bootstrap-ca-cert", required_argument, NULL, OPT_BOOTSTRAP_CA_CERT},
        {NULL, 0, NULL, 0},
    };
    char *short_options = long_options_to_short_options(long_options);

    for (;;) {
        int c;

        c = getopt_long(argc, argv, short_options, long_options, NULL);
        if (c == -1) {
            break;
        }

        switch (c) {
        case 'h':
            usage();

        case 'V':
            ovs_print_version(OFP10_VERSION, OFP10_VERSION);
            exit(EXIT_SUCCESS);

        case OPT_UNIXCTL:
            *unixctl_pathp = optarg;
            break;

        VLOG_OPTION_HANDLERS
        DAEMON_OPTION_HANDLERS
        STREAM_SSL_OPTION_HANDLERS

        case OPT_PEER_CA_CERT:
            stream_ssl_set_peer_ca_cert_file(optarg);
            break;

        case OPT_BOOTSTRAP_CA_CERT:
            stream_ssl_set_ca_cert_file(optarg, true);
            break;

        case '?':
            exit(EXIT_FAILURE);

        default:
            abort();
        }
    }
    free(short_options);

    argc -= optind;
    argv += optind;

    switch (argc) {
    case 0:
        return xasprintf("unix:%s/db.sock", ovs_rundir());

    case 1:
        return xstrdup(argv[0]);

    default:
        VLOG_FATAL("at most one non-option argument accepted; "
                   "use --help for usage");
    }
}

static void
usage(void)
{
    printf("%s: OpenSwitch powerd daemon\n"
           "usage: %s [OPTIONS] [DATABASE]\n"
           "where DATABASE is a socket on which ovsdb-server is listening\n"
           "      (default: \"unix:%s/db.sock\").\n",
           program_name, program_name, ovs_rundir());
    stream_usage("DATABASE", true, false, true);
    daemon_usage();
    vlog_usage();
    printf("\nOther options:\n"
           "  --unixctl=SOCKET        override default control socket name\n"
           "  -h, --help              display this help message\n"
           "  -V, --version           display version information\n");
    exit(EXIT_SUCCESS);
}

static void
ops_powerd_exit(struct unixctl_conn *conn, int argc OVS_UNUSED,
                  const char *argv[] OVS_UNUSED, void *exiting_)
{
    bool *exiting = exiting_;
    *exiting = true;
    unixctl_command_reply(conn, NULL);
}
