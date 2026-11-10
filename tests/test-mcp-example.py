#!/usr/bin/env python3
"""Core MCP example transport smoke test (stdlib, loopback only)."""
import http.client
import json
import signal
import socket
import subprocess
import sys

app=sys.argv[1]
request={'jsonrpc':'2.0','id':1,'method':'tools/list','params':{}}
result=subprocess.run([app,'--stdio'],input=json.dumps(request)+'\n',text=True,capture_output=True,timeout=15)
assert result.returncode==0,result.stderr
stdio=json.loads(result.stdout)
assert len(stdio['result']['tools'])>0
assert subprocess.run([app,'--port','0'],capture_output=True).returncode!=0
with socket.socket() as probe:
    probe.bind(('127.0.0.1',0))
    port=probe.getsockname()[1]
process=subprocess.Popen([app,'--host','127.0.0.1','--port',str(port)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
try:
    assert process.stdout.readline().strip().endswith('/mcp')
    def call(body,session='test'):
        connection=http.client.HTTPConnection('127.0.0.1',port,timeout=10)
        try:
            connection.request('POST','/mcp',json.dumps(body),{'Content-Type':'application/json','Mcp-Session-Id':session})
            response=connection.getresponse()
            return response.status,response.read()
        finally: connection.close()
    status,body=call(request)
    assert status==200,(status,body)
    assert json.loads(body)['result']==stdio['result']
    status,body=call({'jsonrpc':'2.0','method':'notifications/initialized'})
    assert status==202 and body==b'',(status,body)
    connection=http.client.HTTPConnection('127.0.0.1',port,timeout=10)
    try:
        connection.putrequest('POST','/mcp')
        connection.putheader('Content-Length',str(16*1024*1024+1))
        connection.endheaders()
        response=connection.getresponse()
        assert response.status==413
        response.read()
    finally: connection.close()
finally:
    process.send_signal(signal.SIGTERM)
    try: process.communicate(timeout=15)
    except subprocess.TimeoutExpired:
        process.kill();process.communicate();raise
assert process.returncode==0
print('PASS: core MCP example stdio/HTTP dispatch, notification and body limit')
