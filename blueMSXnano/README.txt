INTRO
=====

blueMSXnano is a compact MSX1 emulator for shells like DOS or
bash. The emulator is intended to support enough hardware 
emulation to run MSX basic in a shell. 

By default, the emulator runs the Z80 as  fast as possible 
while keeping a frame rate of 50Hz. Its also possible to run 
the emulation at normal speed, see command line arguments.

The emulation is not complete, but it includes enough features
to run dos programs and cartridges up to 64kB.


USAGE
=====

  Win32, Linux:
    bluemsxnano [-s <bios>] [-r <rom>] [-R <rom>] [-b <rom>] [-v] [-n] [-h]

  MS-DOS:
    bluenano [-s <bios>] [-r <rom>] [-R <rom>] [-b <rom>] [-v] [-n] [-h] [-nodirect]
    
    -s <bios>      Loads a bios rom into address 0x0000
    -R <rom>       Loads a cartridge rom into address 0x0000
    -r <rom>       Loads a cartridge rom into address 0x4000
    -b <rom>       Loads a cartridge rom into address 0x8000
    -v             Verbose, shows FPS and Z80 frequency
    -n             Run emulation in normal speed (3.57MHz)
    -h             Shows help
    -nodirect      (MS-DOS only) Do not use direct access to PC video memory
    
Example:
    Run emulator at normal speed:
    
        bluemsxnano -s msx.rom -n

    Run emulator as fast as possible and show status:
    
        bluemsxnano -s msx.rom -v
    

AUTHORS
=======

Daniel Vik        - Windows port
Wouter Vermaelen  - Linux port 
Juha Riihimaki    - MS-DOS port


(c) 2009 Daniel Vik and the blueMSX team 
