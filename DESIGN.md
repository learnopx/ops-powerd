# High level design of ops-powerd
The ops-powerd daemon monitors the power supplies for the platform.

## Responsibilities
ops-powerd reads presence and state information for power supplies, and reports the information in the database.

## Design choices
ops-powerd does not read power supply information from the FRU EEPROM. This capability may be added in the future.

## Relationships to external OpenSwitch entities
```ditaa
  +------------+     +----------+
  | ops-powerd +---->+  OVSDB   |
  +------------+     +----------+
       |   |
       |   |       +-------+
       |   +------>| PSUs  |
       |           +-------+
       v
  +--------------------+
  |hw description files|
  +--------------------+
```

## OVSDB-Schema
The following rows/cols are created by ops-powerd
```
  rows in power_supply table
  power_supply:name
  power_supply:status
```

The following cols are written by ops-powerd
```
  power_supply:status
  daemon["ops-powerd"]:cur_hw
  subsystem:power_supplies
```

The following cols are read by ops-powerd
```
  subsystem:name
  subsystem:hw_desc_dir
```

## Internal structure
### Main loop
Main loop pseudo-code
```
  initialize OVS IDL
  initialize appctl interface
  while not exiting
  if db has been configured
     process changes to subsystems
     if new subsystem
        allocate data structures
        parse devices file for subsystem
        parse PSU file for subsystem
        for each PSU in subsystem
            read PSU presence and status
            if PSU not in database
                add PSU to database
            set data in PSU
            add PSU to list of PSUs in subsystem
     for each subsystem
        for each PSU in subsystem
           read PSU presence and status
           if change
              update status
        update status LED
  check for appctl
  wait for IDL or appctl input
```

### Source files
```ditaa
  +-----------+
  | powerd.c  |       +---------------------+
  |           |       | config-yaml library |    +----------------------+
  |           +------>+                     +--->+ hw description files |
  |           |       |                     |    +----------------------+
  |           |       |                     |
  |           |       |            +--------+
  |           +------------------> | i2c    |    +------+
  |           |       |            |        +--->+ PSUs |
  |           |       +------------+--------+    +------+
  +-----------+
```

### Data structures
```
locl_subsystem: list of PSUs and their status
locl_psu: PSU data
```
