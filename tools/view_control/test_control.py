import unittest
from unittest.mock import patch
from control import ViewerClient


class ClientTests(unittest.TestCase):
    def test_reject_nonlocal(self):
        for url in ('https://localhost/mcp', 'http://example.com/mcp', 'file:///tmp/a'):
            with self.assertRaises(ValueError):
                ViewerClient(url)

    def test_tool_error_is_not_success(self):
        client = ViewerClient('http://127.0.0.1:8080/mcp')
        with patch.object(client, 'request', return_value={'isError': True, 'content': []}):
            with self.assertRaises(RuntimeError):
                client.call('screenshot')

    def test_json_text_result(self):
        client = ViewerClient('http://localhost:8080/mcp')
        with patch.object(client, 'request', return_value={'content': [{'type': 'text', 'text': '{"ready":true}'}]}):
            self.assertEqual(client.call('status'), {'ready': True})


class ProtocolTests(unittest.TestCase):
    def test_bad_response_identity(self):
        import io
        client = ViewerClient('http://127.0.0.1:8080/mcp')
        with patch.object(client.opener, 'open', return_value=io.BytesIO(b'{"jsonrpc":"2.0","id":true,"result":{}}')):
            with self.assertRaises(RuntimeError):
                client.request('ping')

    def test_rpc_error(self):
        import io
        client = ViewerClient('http://127.0.0.1:8080/mcp')
        with patch.object(client.opener, 'open', return_value=io.BytesIO(b'{"jsonrpc":"2.0","id":1,"error":{"message":"bad"}}')):
            with self.assertRaises(RuntimeError):
                client.request('ping')

    def test_nonfinite_request(self):
        client = ViewerClient('http://localhost:8080/mcp')
        with self.assertRaises(ValueError):
            client.request('tools/call', {'value':float('nan')})

    def test_no_redirect(self):
        from control import NoRedirect
        with self.assertRaises(ValueError):
            NoRedirect().redirect_request(None,None,302,'',{},'http://example.com')

    def test_camera_comparison_ignores_inactive_lens(self):
        from session import camera_key, equivalent
        camera=dict(transform=list(range(16)),projection='orthographic',aspect=1,
                    shift=[0,0],near=.1,far=100,exposure=0,ortho_height=2,fov_y=60)
        other=dict(camera,fov_y=75)
        self.assertTrue(equivalent(camera_key(camera),camera_key(other)))
        other['ortho_height']=3
        self.assertFalse(equivalent(camera_key(camera),camera_key(other)))


class ReportTests(unittest.TestCase):
    def test_report_has_portable_images_and_escapes_metrics(self):
        import tempfile
        from pathlib import Path
        from report import write_report
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            source=root/'source.png'; source.write_bytes(b'png fixture')
            target=root/'report'
            report=Path(write_report(target,source,source,{'note':'</pre><script>bad</script>'}))
            self.assertEqual((target/'reference.png').read_bytes(),source.read_bytes())
            self.assertEqual((target/'candidate.png').read_bytes(),source.read_bytes())
            page=report.read_text()
            self.assertNotIn('</pre><script>bad',page)
            self.assertIn('Wipe',page)
            self.assertIn('reference.png',page)


class SessionFilesTests(unittest.TestCase):
    def test_existing_files_are_not_overwritten(self):
        import tempfile
        from pathlib import Path
        from types import SimpleNamespace
        from session import Session
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            marker=root/'token'; marker.write_text('existing data')
            with self.assertRaises(ValueError): Session(SimpleNamespace(out=root))
            self.assertEqual(marker.read_text(),'existing data')

    def test_token_is_private(self):
        import os
        import tempfile
        from pathlib import Path
        from types import SimpleNamespace
        from session import Session
        with tempfile.TemporaryDirectory() as directory:
            session=Session(SimpleNamespace(out=Path(directory)/'new'))
            self.assertGreaterEqual(len(session.token),16)
            if os.name=='posix':
                self.assertEqual(session.token_file.stat().st_mode & 0o777,0o600)
                self.assertEqual(session.out.stat().st_mode & 0o777,0o700)


class BenchmarkTests(unittest.TestCase):
    def test_failed_timing_resumes_owned_peer(self):
        import os, signal, threading
        from unittest.mock import Mock
        from session import Session
        if os.name!='posix': self.skipTest('POSIX peer suspension')
        state={'camera':dict(transform=[1]*16,projection='perspective',aspect=1,shift=[0,0],near=.1,far=100,exposure=0,fov_y=60),
               'selection':[],'scene':{'loaded':True,'loading':False,'has_animation':False,'filepath':'fixture.usda'},
               'timeline':{'playing':False},'size':[32,32],'frame_serial':1,
               'display':dict(guide=False,proxy=False,render=True,mode='shaded',background='black')}
        def call(tool,args=None):
            if tool=='get_render_stats': raise RuntimeError('injected timing failure')
            return state
        session=Session.__new__(Session)
        session.lock=threading.RLock(); session.error=None
        session.clients={name:Mock(call=call) for name in ('lusdview','usdview')}
        session.children=[Mock(pid=123,poll=Mock(return_value=None)),Mock(pid=456,poll=Mock(return_value=None))]
        session.stopped=threading.Event()
        with patch('session.os.killpg') as kill, patch('session.time.sleep'):
            with self.assertRaisesRegex(RuntimeError,'injected timing failure'): session.benchmark(2,1)
        self.assertEqual(kill.call_args_list[0].args,(456,signal.SIGSTOP))
        self.assertEqual(kill.call_args_list[-1].args,(456,signal.SIGCONT))


class NativeReadbackTests(unittest.TestCase):
    def test_unrepresentable_display_stops_synchronization(self):
        from unittest.mock import Mock
        from session import Session
        requested={'display':dict(guide=True,proxy=False,render=True,mode='shaded',background='black')}
        actual={'display':dict(requested['display'],guide=False)}
        session=Session.__new__(Session)
        session.clients={'lusdview':Mock(call=Mock(return_value=actual))}
        session.last={}; session.revision=0
        with self.assertRaisesRegex(ValueError,'could not represent synchronized display'):
            session.apply('lusdview','display',requested)
        self.assertEqual(session.revision,0)
        self.assertEqual(session.last['lusdview'],actual)


class OverlayRecoveryTests(unittest.TestCase):
    def test_failed_peer_reload_rolls_back_and_does_not_accept(self):
        import threading
        from unittest.mock import Mock
        from session import Session
        state={'scene':{'loading':False,'filepath':'wrapper.usda'},'timeline':{'playing':False}}
        info={'wrapper':'wrapper.usda','overlay':'overlay.usda'}
        usd_operations=[]
        def usd_call(tool,args=None):
            if tool=='view_state': return state
            op=args['op'];usd_operations.append(op)
            if op=='prepare': return {'transaction':'transaction-id'}
            return info
        def lusd_call(tool,args=None):
            if tool=='reload_layer': raise RuntimeError('injected peer reload failure')
            return state
        session=Session.__new__(Session)
        session.lock=threading.RLock();session.error=None;session.overlay=info
        session.clients={'usdview':Mock(call=usd_call),'lusdview':Mock(call=lusd_call)}
        session._wait_scene=Mock();session._restore_views=Mock()
        with self.assertRaisesRegex(RuntimeError,'overlay edit rolled back'):
            session.debug_edit('edit',[{'op':'set_attribute'}])
        self.assertEqual(usd_operations,['prepare','commit','rollback'])
        session._restore_views.assert_called_once()
        self.assertIsNone(session.error)


class PairedToolTests(unittest.TestCase):
    def test_scene_edits_cannot_bypass_shared_overlay(self):
        import threading
        from unittest.mock import Mock
        from types import SimpleNamespace
        from server import dispatch
        native=Mock()
        session=SimpleNamespace(lock=threading.RLock(),clients={'lusdview':native})
        for tool in ('load_usd','prim_create','attr_set','variant_set_selection','debug_overlay'):
            result=dispatch(session,{'jsonrpc':'2.0','id':1,'method':'tools/call',
                'params':{'name':'viewer_call','arguments':{'viewer':'lusdview','tool':tool}}})
            self.assertIn('error',result)
            self.assertIn('use overlay',result['error']['message'])
        native.call.assert_not_called()


class DashboardHTTPTests(unittest.TestCase):
    def handler(self, directory, path, token=None):
        import io
        from types import SimpleNamespace
        from unittest.mock import Mock
        from server import start
        session=SimpleNamespace(out=directory,token='private-session-token')
        with patch('server.ThreadingHTTPServer') as factory, patch('server.threading.Thread'):
            start(session)
        cls=factory.call_args.args[1]
        handler=cls.__new__(cls)
        handler.path=path
        handler.headers={} if token is None else {'Authorization':'Bearer '+token}
        handler.wfile=io.BytesIO()
        handler.send_response=Mock();handler.send_error=Mock()
        handler.send_header=Mock();handler.end_headers=Mock()
        handler.do_GET()
        return handler

    def test_dashboard_contains_no_session_credentials(self):
        import tempfile
        from pathlib import Path
        with tempfile.TemporaryDirectory() as directory:
            handler=self.handler(Path(directory),'/' )
            handler.send_response.assert_called_once_with(200)
            body=handler.wfile.getvalue()
            self.assertIn(b'Shared debug overlay',body)
            self.assertNotIn(b'private-session-token',body)
            self.assertIn(('Content-Security-Policy',"default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; img-src 'self' blob:; frame-ancestors 'none'"),[c.args for c in handler.send_header.call_args_list])

    def test_capture_images_require_auth_and_stay_inside_session(self):
        import tempfile
        from pathlib import Path
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            capture=root/'capture-1';capture.mkdir()
            (capture/'lusdview.png').write_bytes(b'PNG fixture')
            denied=self.handler(root,'/capture/capture-1/lusdview.png')
            denied.send_error.assert_called_once_with(401)
            allowed=self.handler(root,'/capture/capture-1/lusdview.png','private-session-token')
            self.assertEqual(allowed.wfile.getvalue(),b'PNG fixture')
            traversal=self.handler(root,'/capture/../token','private-session-token')
            traversal.send_error.assert_called_once_with(404)
            import os
            if os.name!='posix': return
            outside=root.parent/(root.name+'-outside')
            outside.write_bytes(b'private')
            try:
                (capture/'lusdview.png').unlink()
                (capture/'lusdview.png').symlink_to(outside)
                denied=self.handler(root,'/capture/capture-1/lusdview.png','private-session-token')
                denied.send_error.assert_called_once_with(404)
            finally:
                outside.unlink()


class PlaybackBarrierTests(unittest.TestCase):
    def session(self):
        from unittest.mock import Mock
        from session import Session
        session=Session.__new__(Session)
        session.playback={'active':True,'fps':30,'loop':True,'time':1,'start':1,'end':20,
                          'step':.8,'pending_frames':{'usdview':1,'lusdview':1},'due':0}
        session.last={name:{'scene':{'loading':False},'frame_serial':2,
                            'rendered_time':1,'timeline':{'time':1,'playing':False}}
                      for name in ('usdview','lusdview')}
        session.clients={'usdview':Mock(),'lusdview':Mock()}
        session.apply=Mock()
        return session

    def test_waits_for_each_completed_frame_and_time(self):
        session=self.session()
        session.last['lusdview']['frame_serial']=1
        session._advance_playback();session.apply.assert_not_called()
        session.last['lusdview']['frame_serial']=2
        session.last['lusdview']['rendered_time']=0
        session._advance_playback();session.apply.assert_not_called()
        session.last['lusdview']['rendered_time']=1
        session._advance_playback()
        self.assertEqual(session.apply.call_count,2)
        self.assertEqual(session.playback['time'],1.8)
        for call in session.apply.call_args_list:
            self.assertEqual(call.args[2]['timeline']['time'],1.8)
            self.assertFalse(call.args[2]['timeline']['playing'])

    def test_end_of_nonlooping_playback_stops(self):
        session=self.session();session.playback.update(loop=False,end=1)
        session._advance_playback()
        self.assertFalse(session.playback['active'])
        session.apply.assert_not_called()


class TimePrecisionTests(unittest.TestCase):
    def test_large_time_codes_do_not_acknowledge_a_different_frame(self):
        from session import time_equivalent, states_equal
        self.assertFalse(time_equivalent(1000000,1000001))
        self.assertFalse(time_equivalent(1e15,1e15+1))
        self.assertFalse(time_equivalent(None,1))
        self.assertTrue(time_equivalent(1.2,1.2+1e-12))
        a={'scene':{'has_animation':True},'timeline':{'playing':False,'time':1000000}}
        b={'scene':{'has_animation':True},'timeline':{'playing':False,'time':1000001}}
        self.assertFalse(states_equal(a,b,'timeline'))


class EditSchemaTests(unittest.TestCase):
    def test_controller_advertises_typed_edit_operations(self):
        from server import dispatch
        result=dispatch(None,{'jsonrpc':'2.0','id':1,'method':'tools/list'})
        tools={t['name']:t for t in result['result']['tools']}
        edits=tools['overlay']['inputSchema']['properties']['edits']
        self.assertEqual(edits['maxItems'],128)
        self.assertEqual(edits['items']['required'],['op','path'])
        self.assertIn('variant',edits['items']['properties']['op']['enum'])
        self.assertIn('targets',edits['items']['properties'])
        self.assertIn('playback',tools)


class FullFrameStateTests(unittest.TestCase):
    def state(self):
        return {'camera':dict(transform=list(range(16)),projection='perspective',aspect=1,
                    shift=[0,0],near=.1,far=100,exposure=0,fov_y=60),
                'selection':['/A','/B'],'focus':'/B',
                'timeline':{'time':1,'playing':False},
                'display':dict(guide=False,proxy=False,render=True,mode='shaded',background='black',hud=False),
                'size':[320,240], 'scene':{'filepath':'scene.usda','loaded':True,'loading':False,'has_animation':True,'stage_revision':1}}

    def test_rejects_display_focus_dimension_and_stage_changes(self):
        import copy
        from session import frame_matches
        original=self.state()
        self.assertTrue(frame_matches(copy.deepcopy(original),original))
        changes=(('display','hud',True),('display','mode','wireframe'),
                 ('scene','stage_revision',2),('scene','loading',True),
                 ('timeline','time',2))
        for section,key,value in changes:
            candidate=copy.deepcopy(original);candidate[section][key]=value
            self.assertFalse(frame_matches(candidate,original),(section,key))
        for key,value in (('size',[640,480]),('focus','/A')):
            candidate=copy.deepcopy(original);candidate[key]=value
            self.assertFalse(frame_matches(candidate,original),key)

    def test_focus_is_part_of_synced_selection(self):
        import copy
        from session import states_equal
        original=self.state();other=copy.deepcopy(original)
        other['selection'].reverse()
        self.assertTrue(states_equal(original,other,'selection'))
        other['focus']='/A'
        self.assertFalse(states_equal(original,other,'selection'))


class NativeQueueTests(unittest.TestCase):
    def test_bounded_commands_and_exactly_once_claim(self):
        import json, tempfile
        from pathlib import Path
        from types import SimpleNamespace
        from session import Session
        with tempfile.TemporaryDirectory() as directory:
            session=Session(SimpleNamespace(out=Path(directory)/'session'))
            queue=session.control_requests
            (queue/'a.request.json').write_text('{}')  # Cannot starve valid ids.
            (queue/'1-1.request.json').write_text(json.dumps({'tool':'sync','arguments':{}}))
            reply={'result':{'content':[{'text':'{"done":true}'}]}}
            with patch('server.dispatch',return_value=reply) as dispatch:
                session._process_control_requests()
                session._process_control_requests()
                self.assertEqual(dispatch.call_count,1)
            self.assertTrue(json.loads((queue/'1-1.response.json').read_text())['ok'])
            for identifier,body in [('2-1','null'),('3-1','{"tool":"stop"}'),('4-1','x'*(1024*1024+1))]:
                (queue/(identifier+'.request.json')).write_text(body)
                session._process_control_requests()
                self.assertFalse(json.loads((queue/(identifier+'.response.json')).read_text())['ok'])
            target=Path(directory)/'untrusted';target.write_text('{"tool":"capture"}')
            (queue/'5-1.request.json').symlink_to(target)
            session._process_control_requests()
            self.assertFalse(json.loads((queue/'5-1.response.json').read_text())['ok'])
            self.assertTrue(target.exists())
            self.assertFalse(list(queue.glob('*.processing.json')))


if __name__ == '__main__':
    unittest.main()
