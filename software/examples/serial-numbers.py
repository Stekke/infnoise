#!/usr/bin/python3

import subprocess
import argparse

parser = argparse.ArgumentParser(description='Generate random serial numbers')
parser.add_argument('-l', dest='length', action='store', default=8,
                    help='length of each serial number', type=int)
parser.add_argument('-c', dest='count', action='store', default=10,
                    help='number of serial numbers', type=int)

def main():
    args = parser.parse_args()                                                              # parse arguments
    unique_serials = set()                                                                  # serials generated so far
    while len(unique_serials) < args.count:                                                 # loop until enough unique serials are created (because some could fail)
        serials_list = readHexStrings(args.count - len(unique_serials), args.length)        # read $count hex-strings of fixed $length
        for serial in serials_list:
            if ("\n"  in serial) or (" " in serial):
                continue    # dont use serials with newlines or spaces
            serial = serial.upper()
            if serial not in unique_serials:
                unique_serials.add(serial)
                print(serial)

def readHexStrings(count, length):
    list = []
    infnoise_proc = subprocess.Popen(["infnoise"], stdout=subprocess.PIPE)              # run infinite noise driver and
    bin2hex_proc = subprocess.Popen(["infnoise-bin2hex"], stdin=infnoise_proc.stdout,   # feed its stdout to the bin2hex utility,
                                    stdout=subprocess.PIPE, text=True)                  # you could also skip this and do the bin2hex conversion in python
    while len(list) < count and infnoise_proc.poll() == None:
        list.append(bin2hex_proc.stdout.read(length))
    infnoise_proc.terminate()
    bin2hex_proc.terminate()
    return list

if __name__ == '__main__':
    main()
