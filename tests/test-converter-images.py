#!/usr/bin/env python3
"""Native ordinary-texture precision, resizing and format remapping."""
import pathlib
import struct
import subprocess
import sys
import tempfile
import zipfile
import zlib

converter, cat = sys.argv[1:3]
def chunk(kind, data):
    return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data))
def png16():
    pixel=struct.pack('>HHHH',12345,23456,34567,65535)
    return (b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',8,4,16,6,0,0,0))
            +chunk(b'IDAT',zlib.compress((b'\0'+pixel*8)*4))+chunk(b'IEND',b''))
def run(*args):
    result=subprocess.run(args,capture_output=True,text=True)
    assert result.returncode==0,result.stdout+result.stderr
    return result.stdout
with tempfile.TemporaryDirectory(prefix='lightusd-images-') as temporary:
    directory=pathlib.Path(temporary)
    source=directory/'root.usda'
    source.write_text('#usda 1.0\ndef Shader "Texture" {uniform token info:id="UsdUVTexture"; asset inputs:file=@tex.png@; }\n')
    (directory/'tex.png').write_bytes(png16())
    for fmt in ('png','jpeg'):
        output=directory/(fmt+'.usdz')
        run(converter,str(source),str(output),'-textureFormat',fmt,'-resizeTextures','4')
        with zipfile.ZipFile(output) as archive:
            names=archive.namelist()
            textures=[name for name in names if name.endswith(('.png','.jpg','.jpeg'))]
            assert len(textures)==1,names
            image=archive.read(textures[0])
            if fmt=='png':
                assert struct.unpack_from('>II',image,16)==(4,2)
                assert image[24]==16,'Ordinary PNG processing lost uint16 precision'
            else:
                assert image[:2]==b'\xff\xd8'
                assert textures[0]!='tex.png'
        inspected=directory/(fmt+'.usda')
        run(cat,'--rewrite-layer',str(output),'-o',str(inspected))
        text=inspected.read_text()
        assert '@'+textures[0]+'@' in text,text
print('PASS: native uint16 resize and JPEG format remapping')
