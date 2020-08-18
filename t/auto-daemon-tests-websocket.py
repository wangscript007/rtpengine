import asyncio
import ssl
import websockets
import sys
import unittest
import json
import uuid

# ssl_context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
# ssl_context.check_hostname = False
# ssl_context.verify_mode = ssl.CERT_NONE
# 
# async def do():
#     global args
#     uri = "wss://localhost:7171/"
#     async with websockets.connect(uri, subprotocols=['janus-protocol'], ssl=ssl_context) as websocket:
#         m = args[0]
#         args = args[1:]
#         print(f"sending {m}")
#         await websocket.send(m)
#         msg = await websocket.recv()
#         print(f"{msg}")
# 
# asyncio.get_event_loop().run_until_complete(do())



async def get_ws(cls, proto):
    cls._ws = await websockets.connect("ws://localhost:9191/", \
            subprotocols=[proto])

async def testIO(self, msg):
    await self._ws.send(msg)
    self._res = await self._ws.recv()

async def testIOJson(self, msg):
    await self._ws.send(json.dumps(msg))
    self._res = await self._ws.recv()
    self._res = json.loads(self._res)

async def testIOJanus(self, msg):
    trans = str(uuid.uuid4())
    msg['transaction'] = trans
    await self._ws.send(json.dumps(msg))
    self._res = await self._ws.recv()
    self._res = json.loads(self._res)
    self.assertEqual(self._res['transaction'], trans)
    del self._res['transaction']


class TestWSEcho(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls._eventloop = asyncio.get_event_loop()
        cls._eventloop.run_until_complete(get_ws(cls, 'echo.rtpengine.com'))

    def testEcho(self):
        self._eventloop.run_until_complete(testIO(self, b'foobar'))
        self.assertEqual(self._res, b'foobar')

    def testEchoText(self):
        self._eventloop.run_until_complete(testIO(self, 'foobar'))
        self.assertEqual(self._res, b'foobar')


class TestWSCli(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls._eventloop = asyncio.get_event_loop()
        cls._eventloop.run_until_complete(get_ws(cls, 'cli.rtpengine.com'))

    def testListNumsessions(self):
        self._eventloop.run_until_complete(testIO(self, 'list numsessions'))
        self.assertEqual(self._res, \
                b'Current sessions own: 0\n' +
                b'Current sessions foreign: 0\n' +
                b'Current sessions total: 0\n' +
                b'Current transcoded media: 0\n')


class TestWSJanus(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls._eventloop = asyncio.get_event_loop()
        cls._eventloop.run_until_complete(get_ws(cls, 'janus-protocol'))

    def testPing(self):
        self._eventloop.run_until_complete(testIOJson(self,
                { 'janus': 'ping', 'transaction': 'test123' }))
        self.assertEqual(self._res, \
                { 'janus': 'pong', 'transaction': 'test123' }
        )

    def testPingNoTS(self):
        self._eventloop.run_until_complete(testIOJson(self,
                { 'janus': 'ping' }))
        self.assertEqual(self._res,
                { 'janus': 'error', 'error':
                        { 'code': 456, 'reason': "JSON object does not contain 'transaction' key" } }
        )

    def testInfo(self):
        self._eventloop.run_until_complete(testIOJson(self,
                { 'janus': 'info', 'transaction': 'foobar' }))
        # ignore version string
        self.assertTrue('version_string' in self._res)
        del self._res['version_string']
        self.assertEqual(self._res,
                { 'janus': 'server_info',
                    'name': 'rtpengine Janus interface',
                    'plugins': {'janus.plugin.videoroom': {'name': 'rtpengine Janus videoroom'}},
                    'transaction': 'foobar' }
        )


class TestVideoroom(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls._eventloop = asyncio.get_event_loop()
        cls._eventloop.run_until_complete(get_ws(cls, 'janus-protocol'))

    def testVideoroom(self):
        token = str(uuid.uuid4())

        self._eventloop.run_until_complete(testIOJanus(self,
                { 'janus': 'add_token', 'token': token,
                    'admin_secret': 'YkzAwuEvgLyATjHPvckg' }))
        self.assertEqual(self._res,
                { 'janus': 'success', 'data': {'plugins': ['janus.plugin.videoroom'] }}
        )

        # create session
        self._eventloop.run_until_complete(testIOJanus(self,
                { 'janus': 'create', 'token': token,
                    'admin_secret': 'YkzAwuEvgLyATjHPvckg' }))
        session = self._res['data']['id']
        self.assertIsInstance(session, int)
        self.assertEqual(self._res,
                { 'janus': 'success', 'data': {'id': session }}
        )

        # test keepalive
        self._eventloop.run_until_complete(testIOJanus(self,
                { 'janus': 'keepalive', 'token': token,
                    'session_id': session }))
        self.assertEqual(self._res,
                { 'janus': 'ack', 'session_id': session }
        )

        # attach controlling handle #1
        self._eventloop.run_until_complete(testIOJanus(self,
                { 'janus': 'attach',
                    'plugin': 'janus.plugin.videoroom',
                    'session_id': session,
                    'token': token }))
        handle_c_1 = self._res['data']['id']
        self.assertIsInstance(handle_c_1, int)
        self.assertEqual(self._res,
                { 'janus': 'success', 'session_id': session, 'data': {'id': handle_c_1 }}
        )

        # create room
        self._eventloop.run_until_complete(testIOJanus(self,
                { 'janus': 'message',
                    'body': { 'request': 'create', 'publishers': 16 },
                    'handle_id': handle_c_1,
                    'session_id': session,
                    'token': token }))
        room = self._res['plugindata']['data']['room']
        self.assertIsInstance(room, int)
        self.assertEqual(self._res,
                { 'janus': 'success',
                    'session_id': session,
                    'sender': handle_c_1,
                    'plugindata':
                     { 'plugin': 'janus.plugin.videoroom',
                       'data':
                        { 'videoroom': 'created',
                          'room': room,
                          'permanent': False } } })



if __name__ == '__main__':
    unittest.main()
