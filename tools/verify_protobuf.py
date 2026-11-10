"""Check the independent wire codec against the official protoc; no Python deps."""
import argparse
import pathlib
import subprocess
import struct
import sys
import tempfile
import zlib

p = argparse.ArgumentParser()
p.add_argument("--protoc", required=True)
p.add_argument("--tester", required=True)
args = p.parse_args()
root = pathlib.Path(__file__).resolve().parents[1]
fixture = b'''maximum: 18446744073709551615
minimum: -9223372036854775808
bits32: 305419896
bits64: 1311768467463790320
binary: "A\\000\\377"
single: 1.25
real: -2.5
packed: [0, 127, 128, 4294967295]
child { value: 150 }
'''
def protoc(action, data, schema="wire_fixture.proto", message="fengqudb.test.WireFixture"):
    return subprocess.run([args.protoc, f"--{action}={message}", f"-I{root / 'tests'}", f"-I{root / 'proto'}", schema], input=data, capture_output=True, check=True).stdout

with tempfile.TemporaryDirectory(prefix="fqdb-protobuf-") as directory:
    path = pathlib.Path(directory) / "wire.bin"
    encoded = protoc("encode", fixture)
    path.write_bytes(encoded)
    subprocess.run([args.tester, "--wire-check", str(path)], check=True)
    subprocess.run([args.tester, "--wire-emit", str(path)], check=True)
    ours = path.read_bytes()
    assert ours == encoded, (ours.hex(), encoded.hex())
    assert protoc("decode", ours) == protoc("decode", encoded)
    # Unknown length-delimited field #100 and a legacy group #101.
    path.write_bytes(encoded + bytes.fromhex("a2060378797aab060801ac06"))
    subprocess.run([args.tester, "--wire-check", str(path)], check=True)
    protoc("decode", path.read_bytes())
    print(f"PASS official protoc -> FengquDB -> official protoc ({len(ours)} bytes), packed/nested/unknown fields")
