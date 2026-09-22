"""Cross-check the embedded wire codec against the real Cap'n Proto runtime."""
import pathlib
import random
import struct
import subprocess
import unittest

import capnp

ROOT = pathlib.Path(__file__).resolve().parents[1]
CAPNP = pathlib.Path(capnp.__file__).parent
RPC = capnp.load(str(CAPNP/'rpc.capnp'), imports=[str(CAPNP.parent)])
TUNNEL = capnp.load(str(ROOT/'tests/schema/tunnelrpc.capnp'))
CLI = ROOT/'tests/build/rpc_cli'

def invoke(mode, data=b''):
    return subprocess.run([str(CLI), mode], input=data, capture_output=True)

def reply(error=False, tiny=False):
    m = RPC.Message.new_message(num_first_segment_words=1 if tiny else 1024)
    ret = m.init('return')
    ret.answerId = 1
    result = TUNNEL.RegistrationResult.new_message()
    union = result.init('result').result
    if error:
        union.init('error').cause = 'test rejection'
    else:
        details = union.init('connectionDetails')
        details.locationName = 'SJC'
        details.uuid = b'u'*16
        details.tunnelIsRemotelyManaged = True
    ret.init('results').content = result
    return m.to_bytes()

class CodecTests(unittest.TestCase):
    def test_registration_matches_official_schema(self):
        p = invoke('registration')
        self.assertEqual(p.returncode, 0)
        with RPC.Message.from_bytes(p.stdout) as m:
            self.assertEqual(m.which(), 'call')
            self.assertEqual(m.call.questionId, 1)
            self.assertEqual(m.call.interfaceId, 0xf71695ec7fe85497)
            self.assertEqual(m.call.methodId, 0)
            self.assertEqual(m.call.target.promisedAnswer.questionId, 0)
            params = m.call.params.content.as_struct(TUNNEL.RegistrationServer.schema.methods['registerConnection'].param_type)
            self.assertEqual(params.auth.accountTag, 'a'*32)
            self.assertEqual(params.auth.tunnelSecret, b'S'*32)
            self.assertEqual(params.tunnelId, b'T'*16)
            self.assertEqual(params.options.client.clientId, b'C'*16)
            self.assertEqual(list(params.options.client.features), ['allow_remote_config', 'serialized_headers'])
            self.assertEqual(params.options.originLocalIp, bytes([192,168,1,2]))

    def test_four_connection_indices(self):
        for index in range(4):
            p = subprocess.run([str(CLI), 'registration', str(index)], capture_output=True)
            self.assertEqual(p.returncode, 0, p.stderr)
            with RPC.Message.from_bytes(p.stdout) as m:
                params = m.call.params.content.as_struct(TUNNEL.RegistrationServer.schema.methods['registerConnection'].param_type)
                self.assertEqual(params.connIndex, index)
                self.assertEqual(params.options.client.clientId, b'C'*16)
        self.assertEqual(subprocess.run([str(CLI), 'registration', '4'], capture_output=True).returncode, 1)

    def test_bootstrap(self):
        with RPC.Message.from_bytes(invoke('bootstrap').stdout) as m:
            self.assertEqual(m.which(), 'bootstrap')
            self.assertEqual(m.bootstrap.questionId, 0)

    def test_success_and_multisegment(self):
        for tiny in [False, True]:
            p = invoke('parse', reply(tiny=tiny))
            self.assertEqual((p.returncode, p.stdout), (0, b'1:SJC'), p.stderr)

    def test_rejected_registration(self):
        p = invoke('parse', reply(error=True))
        self.assertEqual((p.returncode,p.stdout), (0,b'2:test rejection'))

    def test_rpc_exception(self):
        m = RPC.Message.new_message()
        ret = m.init('return'); ret.answerId = 1
        ret.init('exception').reason = 'test exception'
        p = invoke('parse', m.to_bytes())
        self.assertEqual((p.returncode,p.stdout), (0,b'2:test exception'))

    def test_fragmentation(self):
        b = reply()
        for n in [0,1,3,4,7,8,15,20,len(b)-1]:
            p = invoke('parse', b[:n])
            self.assertEqual((p.returncode,p.stdout), (0,b'incomplete'))

    def test_segment_limits(self):
        for b in [struct.pack('<I',16), struct.pack('<II',0,1000000)]:
            self.assertEqual(invoke('parse',b).returncode,1)

    def test_untrusted_pointer_mutations(self):
        rng = random.Random(0)
        original = reply(tiny=True)
        for _ in range(100):
            b = bytearray(original)
            pos = rng.randrange(8,len(b))
            b[pos] ^= rng.randrange(1,256)
            p = invoke('parse',b)
            self.assertIn(p.returncode, [0,1], p.stderr)
            self.assertNotIn(b'Sanitizer',p.stderr)
            self.assertNotIn(b'runtime error:',p.stderr)

if __name__ == '__main__':
    unittest.main()
