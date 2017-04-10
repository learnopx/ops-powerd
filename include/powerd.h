/*
 * (c) Copyright 2015 Hewlett Packard Enterprise Development LP
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
 * @defgroup ops-powerd Power Supply Daemon
 * This module is the platform daemon that processess and manages power
 * supplies for all subsystems in the switch that have manageable
 * modular power supplies.
 *
 * @{
 *
 * @file
 * Header for platform Power Supply daemon
 *
 * @defgroup powerd_public Public Interface
 * Public API for the platform Power Supply daemon
 *
 * The platform Power Supply daemon is responsible for managing and reporting
 * status for power supplies in any subsystem that has power supplies that can
 * be managed or reported.
 *
 * @{
 *
 * Public APIs
 *
 * Command line options:
 *
 *     usage: ops-powerd [OPTIONS] [DATABASE]
 *     where DATABASE is a socket on which ovsdb-server is listening
 *           (default: "unix:/var/run/openvswitch/db.sock").
 *
 *     Active DATABASE connection methods:
 *          tcp:IP:PORT             PORT at remote IP
 *          ssl:IP:PORT             SSL PORT at remote IP
 *          unix:FILE               Unix domain socket named FILE
 *     PKI configuration (required to use SSL):
 *          -p, --private-key=FILE  file with private key
 *          -c, --certificate=FILE  file with certificate for private key
 *          -C, --ca-cert=FILE      file with peer CA certificate
 *          --bootstrap-ca-cert=FILE  file with peer CA certificate to read or create
 *
 *     Daemon options:
 *          --detach                run in background as daemon
 *          --no-chdir              do not chdir to '/'
 *          --pidfile[=FILE]        create pidfile (default: /var/run/openvswitch/ops-powerd.pid)
 *          --overwrite-pidfile     with --pidfile, start even if already running
 *
 *     Logging options:
 *          -vSPEC, --verbose=SPEC   set logging levels
 *          -v, --verbose            set maximum verbosity level
 *          --log-file[=FILE]        enable logging to specified FILE
 *                                  (default: /var/log/openvswitch/ops-powerd.log)
 *          --syslog-target=HOST:PORT  also send syslog msgs to HOST:PORT via UDP
 *
 *     Other options:
 *          --unixctl=SOCKET        override default control socket name
 *          -h, --help              display this help message
 *          -V, --version           display version information
 *
 *
 * ovs-apptcl options:
 *
 *      Support dump: ovs-appctl -t ops-powerd ops-powerd/dump
 *
 *
 * OVSDB elements usage
 *
 *     Creation: The following rows/cols are created by ops-powerd
 *               rows in Power_supply table
 *               Power_supply:name
 *               Power_supply:status
 *
 *     Written: The following cols are written by ops-powerd
 *              Power_supply:status
 *              subsystem:power_supplies
 *              daemon["ops-powerd"]:cur_hw
 *
 *     Read: The following cols are read by ops-powerd
 *           subsystem:name
 *           subsystem:hw_desc_dir
 *
 * Linux Files:
 *
 *     The following files are written by ops-powerd
 *           /var/run/openvswitch/ops-powerd.pid: Process ID for the ops-powerd daemon
 *           /var/run/openvswitch/ops-powerd.<pid>.ctl: unixctl socket for the ops-powerd daemon
 *
 * @}
 ***************************************************************************/


#ifndef _POWERD_H_
#define _POWERD_H_

#include <stdbool.h>
#include "shash.h"
#include "config-yaml.h"

VLOG_DEFINE_THIS_MODULE(ops_powerd);

COVERAGE_DEFINE(powerd_reconfigure);

#define NAME_IN_DAEMON_TABLE "ops-powerd" /*!< Name of daemon */

#define POLLING_PERIOD  5     /*!< polling period in seconds */
#define MSEC_PER_SEC    1000  /*!< number of miliseconds in a second */

/* psu status reported in DB (must match psu_status string array, below) */
/************************************************************************//**
 * ENUM containing possible values for the power supply status
 ***************************************************************************/
enum psustatus {
    PSU_STATUS_OVERRIDE_NONE = -1, /*!< Override value */
    PSU_STATUS_OK = 0,             /*!< value for OK */
    PSU_STATUS_FAULT_INPUT = 1,    /*!< value for input fault */
    PSU_STATUS_FAULT_OUTPUT = 2,   /*!< value for output fault */
    PSU_STATUS_FAULT_ABSENT = 3,   /*!< value for absent fault */
    PSU_STATUS_UNKNOWN = 4         /*!< value for status unknown */
};

/* must match psustatus enum */
/************************************************************************//**
 * Char_array containing string values for the power supply status
 ***************************************************************************/
const char *psu_status[] =
{
    "ok",            /*!< string value for PSU_STATUS_OK */
    "fault_input",   /*!< string value for PSU_STATUS_FAULT_INPUT */
    "fault_output",  /*!< string value for PSU_STATUS_FAULT_OUTPUT */
    "fault_absent",  /*!< string value for PSU_STATUS_FAULT_ABSENT */
    "unknown"        /*!< string value for PSU_STATUS_UNKNOWN */
};

/************************************************************************//**
 * STRUCT containing local copy of info for a subsystem
 ***************************************************************************/
struct locl_subsystem {
    char *name;             /*!< name identifier of the subsystem */
    bool marked;            /*!< flag for calculating "in use" status */
    bool valid;             /*!< flag to know if this is a valid subsys */
    enum psustatus status;  /*!< current power supply status */
    struct locl_subsystem *parent_subsystem; /*!< pointer to parent (if any) */
    struct shash subsystem_psus;  /*!< power supplies in this subsystem */
};

/************************************************************************//**
 * STRUCT containing local copy of info for a power supply
 ***************************************************************************/
struct locl_psu {
    char *name;             /*!< name of psu ([subsystem name]-[psu number]) */
    struct locl_subsystem *subsystem;   /*!< containing subsystem */
    const YamlPsu *yaml_psu;    /*!< psu information */
    enum psustatus status;      /*!< current status result */
    enum psustatus test_status; /*!< status override for test */
};

/************************************************************************//**
 * DEFINE for maximum i2c retries on failure
 ***************************************************************************/
#define MAX_FAIL_RETRY  2

/************************************************************************//**
 * ENUM containing possible values for i2c operation status
 ***************************************************************************/
enum bit_op_result {
    BIT_OP_FAIL,        /*!< could not execute bit operation */
    BIT_OP_STATUS_OK,   /*!< result is ok */
    BIT_OP_STATUS_BAD   /*!< result is fault */
};

#endif /* _POWERD_H_ */
/** @} end of group ops-powerd */
