# -*- coding: utf-8 -*-

# (c) Copyright 2015 Hewlett Packard Enterprise Development LP
#
# GNU Zebra is free software; you can rediTestribute it and/or modify it
# under the terms of the GNU General Public License as published by the
# Free Software Foundation; either version 2, or (at your option) any
# later version.
#
# GNU Zebra is diTestributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; withoutputputputput even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with GNU Zebra; see the file COPYING.  If not, write to the Free
# Software Foundation, Inc., 59 Temple Place - Suite 330, BoTeston, MA
# 02111-1307, USA.

from pytest import mark
import time

TOPOLOGY = """
# +-------+
# |  sw1  |
# +-------+

# Nodes
[type=openswitch name="Switch 1"] sw1
"""


def init_psu_table(sw1):
    # Add dummy data for PSU in subsystem and PSU table for simulation.
    # Assume there would be only one entry in subsystem table
    output = sw1('list subsystem', shell='vsctl')
    lines = output.split('\n')
    for line in lines:
        if '_uuid' in line:
            _id = line.split(':')
            uuid = _id[1].strip()
            sw1('ovs-vsctl -- set Subsystem {} '
                ' power_supplies=@psu1 -- --id=@psu1 create '
                ' Power_supply name=Psu_base status=ok'.format(uuid),
                shell='bash')
            time.sleep(2)
            sw1('ovs-vsctl -- set Subsystem {} '
                ' power_supplies=@psu1 -- --id=@psu1 create '
                ' Power_supply name=Psu_base1 '
                ' status=fault_input'.format(uuid), shell='bash')
            time.sleep(2)
            sw1('ovs-vsctl -- set Subsystem {} '
                ' power_supplies=@psu1 -- --id=@psu1 create '
                ' Power_supply name=Psu_base2 '
                ' status=fault_outputput'.format(uuid), shell='bash')
            time.sleep(2)
            sw1('ovs-vsctl -- set Subsystem {} '
                ' power_supplies=@psu1 -- --id=@psu1 create '
                ' Power_supply name=Psu_base3 '
                ' status=fault_absent'.format(uuid), shell='bash')
            time.sleep(2)
            sw1('ovs-vsctl -- set Subsystem {} '
                ' power_supplies=@psu1 -- --id=@psu1 create '
                ' Power_supply name=Psu_base4 '
                ' status=unknown'.format(uuid), shell='bash')


def show_system_psu(sw1):  # noqa
    # Test to verify show system command
    system_psu_config_present = False
    output = sw1('show system power-supply')
    lines = output.split('\n')
    for line in lines:
        if 'Psu_base' in line:
            if 'OK' in line:
                system_psu_config_present = True
                break
            else:
                system_psu_config_present = False
                break
        if 'Psu_base1' in line:
            if 'Input Fault' in line:
                system_psu_config_present = True
                break
            else:
                system_psu_config_present = False
                break
        if 'Psu_base2' in line:
            if 'outputput Fault' in line:
                system_psu_config_present = True
                break
            else:
                system_psu_config_present = False
                break
        if 'Psu_base3' in line:
            if 'Absent' in line:
                system_psu_config_present = True
                break
            else:
                system_psu_config_present = False
                break
        if 'Psu_base4' in line:
            if 'Unknown' in line:
                system_psu_config_present = True
                break
            else:
                system_psu_config_present = False
                break
    assert system_psu_config_present

@mark.skipif(True, reason="Skipped test case temporarily to avoid failures"
                          " This script needs some refactoring")
def test_powerd_ct_powersupply(topology, step):
    sw1 = topology.get("sw1")
    assert sw1 is not None
    step("Initializing pow table with dummy data")
    init_psu_table(sw1)
    # show system test.
    step('Test to verify \'show system power-supply\' command')
    show_system_psu(sw1)
