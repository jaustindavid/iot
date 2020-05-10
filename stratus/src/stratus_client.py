#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import hashlib
import http.client
import re

debuggery = False
server_debuggery = False

"""
action: config
   guid: me

action: publish
   key: string
   value: string
   ttl: int
   scope: string (PRIVATE or something else)
   guid: me

action: subscribe (proper subset of publish)
   key: string
   scope: as above
   guid: me
"""

class StratusMessage:
    contents = dict()

    def __init__(self, action="echo", key="", value="", guid="guid", scope="PRIVATE", ttl=60, limit=0, reset=0):
        self.contents['action'] = action
        self.contents['guid'] = guid
        self.contents['key'] = key
        self.contents['value'] = value
        self.contents['scope'] = scope
        self.contents['ttl'] = ttl
        self.contents['limit'] = limit
        self.contents['reset'] = "1" if reset else "0"


    def to_url(self, secret):
        # generate signature
        body = f'{secret}\nguid: {self.contents["guid"]}\n{self.contents["key"]}: {self.contents["value"]}\n'
        md5 = hashlib.md5(body.encode('utf-8')).hexdigest()
        ret = '&'.join([f'{key}={value}' for key, value in self.contents.items()])
        ret += f'&signature={md5}'
        return ret
        

class StratusClient:
    config_url = ""
    host = ""
    path = ""
    secret = "stratus secret key"
    guid = "PyClient"
    subscriptions = dict()

    def __init__(self, url, secret, guid="PyClient"):
        self.set_config_url(url)
        self.secret = secret 
        self.guid = guid


    def set_config_url(self, url):
        self.config_url = url
        result = re.match(r'.*//(.*?)/(.*$)', self.config_url)
        self.host = result.group(1)
        self.path = "/" + result.group(2)


    def validate(self, data):
        sig_locn = data.find("\nsignature: ") + len("\n")
        body = data[:sig_locn]
        if debuggery: print(f'body:>>>>>>>>\n{body}<<<<<<<<')
        signature = data[sig_locn + len("signature: "):-len("\n")]
        if debuggery: print(f'signature: {signature}')
        md5 = hashlib.md5(f'{self.secret}\n{body}'.encode('utf-8')).hexdigest()
        if debuggery: print(f'MD5: {md5}')
        debug_locn = body.find("\n<<<<<>>>>>\n");
        if server_debuggery: print(f'DEBUG:\n{data[debug_locn + 12:sig_locn]}')
        body = body[:debug_locn];
        if (md5 == signature):
            if debuggery: print("Matches!")
            return body
        else:
            if debuggery: print("Unable to validate")
            return False


    def send(self, message):
        url = f'{self.path}?' + message.to_url(self.secret)
        if debuggery: print(f'sending >>{url}<<')
        conn = http.client.HTTPConnection(self.host)
        conn.request("GET", url)
        response = conn.getresponse()
        # TODO if r1.status == 200
        data = response.read().decode('utf-8')
        conn.close()
        if debuggery: print(f'got back: >>\n{data}<<')
        return self.validate(data)
        

    # synchronous: send a message to the server NOW
    def publish(self, key, value, scope="PRIVATE", ttl=60):
        message = StratusMessage("publish", key, value, self.guid, scope, ttl)
        result = self.send(message)
        # print(f'Publish got:>>>>>>>>>\n{result}<<<<<<<<<<<<<')
        return result and result.startswith("result: success")


    # asynchronous: just record the fact that I am interested
    def subscribe(self, callback, key, scope="PRIVATE", reset=False, limit=0):
        if (key not in self.subscriptions):
            self.subscriptions[key] = dict()
        if (scope not in self.subscriptions[key]):
            self.subscriptions[key][scope] = dict()
        self.subscriptions[key][scope]['callback'] = callback
        self.subscriptions[key][scope]['reset'] = reset
        self.subscriptions[key][scope]['limit'] = limit
        # message = StratusMessage("subscribe", key, "", self.guid, scope)
        # return self.send(message)


    def unsubscribe(self, key, scope="PRIVATE"):
        del self.subscriptions[key][scope];


    def handle_callback(self, response, message):
        if response and response.startswith("result: success\n"):
            response = response[len("result: success\n"):]
            if len(response) > 0:
                if debuggery: print(f'I should handle {response}')
                key = message.contents['key']
                scope = message.contents['scope']
                for value in response.split(key + ": "):
                    if len(value):
                        if debuggery: print(f'handling: {key} :: {value.strip()}')
                        self.subscriptions[key][scope]['callback'](key=key, value=value.strip())


    # retrieve any held subscription info
    # todo: publish queued messages
    def update(self):
        if debuggery: 
            print("update()")
            print(self.subscriptions)
        for key in self.subscriptions:
            for scope in self.subscriptions[key]:
                if debuggery: print(f'Subcribed to {scope}::{key}')
                message = StratusMessage(action="subscribe", 
                                 key=key, guid=self.guid, scope=scope,
                                 limit=self.subscriptions[key][scope]['limit'],
                                 reset=self.subscriptions[key][scope]['reset'])
                response = self.send(message)
                if debuggery: print(f'got response: >>>>\n{response}<<<<<<<')
                # remove limit, reset
                self.subscriptions[key][scope]['reset'] = False
                self.subscriptions[key][scope]['limit'] = 0
                self.handle_callback(response, message)

# end class StratusClient
