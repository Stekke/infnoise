#!/usr/bin/python3

import queue
import shlex
import subprocess
from threading import Thread
import web

urls = (
    '/', 'help',
    '/get', 'GetRandomData',
#    '/reg', 'Register',
    '/status', 'GetPoolStatus'
)

RESPONSE_SIZE=8192

MIN_POOL_SIZE=1  # in MB
MAX_POOL_SIZE=32 # in MB

# There is no TLS, so anybody on the network path could read the random data
LISTEN_ADDRESS=("127.0.0.1", 8080)

class help:
    def GET(self):
        message= "<html><head>"
        message+= "<title>Infinite Noise webservice</title></head>"
        message+= "<body><h1>Usage:</h1><table border=1>"
        message+= "<tr><td><b>request</b></td><td><b>help</b></td></tr>"
        message+= "<tr><td>/status</td><td>show current buffer status</td></tr>"
        message+= "<tr><td>/get</td><td>get " + str(RESPONSE_SIZE) + "bytes (binary format)</td></tr>"
#        print "<td><tr>/reg</tr><tr>todo</tr></td>"
        message+= "</table>"
        message+= "</body></html>"
        return message

class GetRandomData:
    def GET(self):
        return POOL.getData()

class GetPoolStatus:
    def GET(self):
        return POOL.getStatus()

class RandPool(object):
    minPoolSize = (MIN_POOL_SIZE * 1024 * 1024) // RESPONSE_SIZE
    maxPoolSize = (MAX_POOL_SIZE * 1024 * 1024) // RESPONSE_SIZE

    def __init__(self):
        # The queue makes sure that every block is handed out once, also to parallel requests
        self.randomPool = queue.Queue(self.maxPoolSize)
        command = "sudo /usr/sbin/infnoise --multiplier 10"
        self.process = subprocess.Popen(shlex.split(command), stdout=subprocess.PIPE)
        for i in range(0, self.minPoolSize):
            self.__read_block__()
        thread2 = Thread(target=self.monitorPool, daemon=True)
        thread2.start()

    def __read_block__(self):
        data = self.process.stdout.read(RESPONSE_SIZE)
        if len(data) < RESPONSE_SIZE:
            return False
        self.randomPool.put(data) # waits while the pool is at 100%
        return True

    def monitorPool(self):
        while self.__read_block__():
            pass
        print("infnoise stopped")

    def getData(self):
        try:
            return self.randomPool.get_nowait()
        except queue.Empty:
            web.ctx.status = '503 Service currently unavailable - Out of entropy error'
            return "0"

    def getStatus(self):
        return str(self.randomPool.qsize()) + "/" + str(self.maxPoolSize)

    def stop(self):
        print("Stopping as you wish.")
        self.process.kill()

POOL = RandPool()

if __name__ == '__main__':
    app = web.application(urls, globals())
    web.httpserver.runsimple(app.wsgifunc(), LISTEN_ADDRESS)
    POOL.stop()
