"""Remove a test-owned Store only after validating its complete file group."""
import argparse
import json
import os
from pathlib import Path
import re
import stat
import struct
import zlib

class FixtureError(RuntimeError):
    pass

def need(condition, message):
    if not condition:
        raise FixtureError(message)

def varint(data, offset):
    value=0
    for index in range(10):
        need(offset<len(data),'truncated CURRENT varint')
        byte=data[offset];offset+=1
        need(index!=9 or byte<=1,'overflow CURRENT varint')
        value|=(byte&127)<<(7*index)
        if not byte&128:return value,offset
    raise FixtureError('overlong CURRENT varint')

def current(path):
    data=path.read_bytes()
    need(32<=len(data)<=4096 and data[:8]==b'FQCUR004','invalid CURRENT')
    version,length=struct.unpack_from('<II',data,8)
    generation,payload_crc,header_crc=struct.unpack_from('<QII',data,16)
    need(version==4 and length==len(data)-32,'invalid CURRENT length/version')
    need(zlib.crc32(data[:28])==header_crc and zlib.crc32(data[32:])==payload_crc,'CURRENT CRC')
    values={};offset=32
    while offset<len(data):
        tag,offset=varint(data,offset);field,wire=tag>>3,tag&7
        need(field in (1,2,3) and field not in values,'unknown/duplicate CURRENT field')
        if wire==2:
            size,offset=varint(data,offset);need(size<=len(data)-offset,'CURRENT bytes overflow')
            value=data[offset:offset+size];offset+=size
        else:
            need(wire==0,'CURRENT wire type');value,offset=varint(data,offset)
        values[field]=value
    need(set(values)=={1,2,3} and isinstance(values[1],bytes) and len(values[1])==16,'CURRENT identity')
    need(values[2]==generation and generation>0 and isinstance(values[3],bytes),'CURRENT generation')
    filename=values[3].decode('ascii')
    expected=[f'g{generation:016x}.fdb',f'gen-{values[1].hex()}-{generation:016x}.fdb']
    need(filename in expected,'noncanonical CURRENT filename')
    return values[1],generation,filename

def block(path, offset, size):
    with path.open('rb') as stream:
        stream.seek(offset);data=stream.read(size)
    need(len(data)==size,'truncated identity header')
    return data

def data_header(path, offset, uuid, generation):
    header=block(path,offset,64)
    need(header[:8]==b'FQDATA04' and struct.unpack_from('<II',header,8)==(4,64),'Store data identity')
    need(zlib.crc32(header[:60])==struct.unpack_from('<I',header,60)[0],'Store data CRC')
    need(header[16:32]==uuid and struct.unpack_from('<Q',header,32)[0]==generation,'Store UUID/generation')

def segment_header(path, uuid, generation, segment, geometry=None):
    both=block(path,0,128);header=both[:64]
    need(header==both[64:] and header[:8]==b'FQSEG002','segment identity mirrors')
    need(zlib.crc32(header[:60])==struct.unpack_from('<I',header,60)[0],'segment identity CRC')
    capacity,number,part=struct.unpack_from('<QQI',header,8)
    need(65536<=capacity<=1<<30 and capacity&(capacity-1)==0,'segment capacity')
    need(geometry is None or capacity==geometry,'inconsistent segment capacity')
    need(number==generation and part==segment and header[32:48]==uuid,'foreign segment identity')
    need(65536<=path.stat().st_size<=65536+capacity,'segment physical length')
    return capacity

def hybrid_header(path, uuid, generation, magic):
    both=block(path,0,128);header=both[:64]
    need(header==both[64:] and header[:8]==magic,'hybrid identity mirrors')
    need(struct.unpack_from('<I',header,8)[0]==1,'hybrid identity version')
    need(zlib.crc32(header[:60])==struct.unpack_from('<I',header,60)[0],'hybrid identity CRC')
    need(header[16:32]==uuid and struct.unpack_from('<Q',header,32)[0]==generation,'hybrid UUID/generation')
    if magic==b'FQAHYB01':
        need(struct.unpack_from('<I',header,12)[0]==4032 and struct.unpack_from('<II',header,40)==(4,4096),'hybrid page geometry')
        need(path.stat().st_size>=20480 and (path.stat().st_size-4096)%16384==0,'hybrid cell length')

def inspect_owned_store(path, owner_root):
    path,owner_root=Path(os.path.abspath(path)),Path(owner_root).resolve(strict=True)
    need(path.is_relative_to(owner_root),'noncanonical owned path')
    need(path.resolve(strict=True).is_relative_to(owner_root) and path.resolve()!=owner_root,'outside owned root')
    cursor=path
    while cursor!=owner_root:
        need(not cursor.is_symlink() and not cursor.is_junction(),'reparse fixture path')
        cursor=cursor.parent
    files=list(path.iterdir())
    for entry in files:
        info=entry.lstat()
        need(stat.S_ISREG(info.st_mode) and not entry.is_symlink(),'nonregular fixture entry')
    names={entry.name for entry in files}
    need('CURRENT' in names and 'LOCK' in names,'missing Store controls')
    uuid,generation,primary=current(path/'CURRENT')
    need(primary in names,'missing selected generation')
    first=block(path/primary,0,8);capacity=None;companion=None
    if first==b'FQSEG002':
        capacity=segment_header(path/primary,uuid,generation,0)
        data_header(path/primary,65536,uuid,generation)
    elif first==b'FQAHYB01':
        hybrid_header(path/primary,uuid,generation,b'FQAHYB01')
        companion=Path(primary).with_suffix('.blk').name
        need(companion in names,'missing hybrid companion')
        hybrid_header(path/companion,uuid,generation,b'FQHYLG01')
    else:data_header(path/primary,0,uuid,generation)
    for entry in files:
        if entry.name=='WRITE':
            policy=entry.read_bytes()
            need(len(policy)==40 and policy[:8]==b'FQWRITE1','invalid WRITE policy')
            need(zlib.crc32(policy[:36])==struct.unpack_from('<I',policy,36)[0],'WRITE CRC')
            continue
        if entry.name in ('CURRENT','LOCK',primary,companion):continue
        need(capacity is not None,'unexpected file beside legacy generation')
        match=re.fullmatch(r's([0-9a-f]{16})_([0-9a-f]{8})\.fdb',entry.name)
        need(match is not None and int(match[1],16)==generation and int(match[2],16)>0,'unknown fixture file')
        segment_header(entry,uuid,generation,int(match[2],16),capacity)
    return files,{'files':len(files),'segments':len(files)-2,'geometry':capacity or 0,
                  'max_filename_chars':max(map(len,names))}

def cleanup_owned_store(path, owner_root):
    files,info=inspect_owned_store(path,owner_root)
    # All checks precede deletion. Keep the controls until data removal succeeds.
    files.sort(key=lambda p:p.name in ('CURRENT','LOCK'))
    for entry in files:entry.unlink()
    Path(path).rmdir()
    return info

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--owner-root',required=True,type=Path)
    parser.add_argument('--store',required=True,type=Path);args=parser.parse_args()
    print(json.dumps(cleanup_owned_store(args.store,args.owner_root)),flush=True)
