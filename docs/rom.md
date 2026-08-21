# Base ROM

Target dump (the only one used for analysis):

    Dune - The Battle for Arrakis (E).bin
    size   1048576
    md5    a0797d35cfb48d68ebb8b977d057ba3e
    sha1   133cc86b43afe133fc9c9142b448340c17fa668e

Header checksum 0x5E34 matches computed value -> clean dump.

Other files in roms/ are bad dumps ([b1],[b2]), header hacks ([h1]-[h3]),
or the Russian fan translation ([T+Rus], 2 MiB). None are used.

## Header

    console     SEGA MEGA DRIVE
    copyright   (C)T-70 1994.JAN
    title       DUNE - THE BATTLE FOR ARRAKIS
    serial      GM T-70246 -00
    region      E (PAL Europe)
    io          J (3-button pad)
    ROM         0x000000-0x0FFFFF
    RAM         0xFF0000-0xFFFFFF
    SRAM        none
    init SSP    0xFFFFFFFA
    reset PC    0x000200
