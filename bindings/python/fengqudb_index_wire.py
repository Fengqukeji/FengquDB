"""Dependency-free wire codec for the owning index module's proto/index.proto v1."""

import math
import struct
from dataclasses import dataclass, field
from enum import IntEnum

MAX_MESSAGE = 16 * 1024 * 1024


class WireError(ValueError):
    pass


class Mutation(IntEnum):
    UPSERT = 1
    PATCH = 2
    ERASE = 3


class Query(IntEnum):
    SEARCH = 1
    GET = 2
    STATS = 3
    LIST = 4


@dataclass(frozen=True)
class UInt:
    value: int


@dataclass(frozen=True)
class Predicate:
    operation: int = 0
    field: str = ""
    values: tuple = ()
    children: tuple = ()


@dataclass
class Document:
    id: bytes
    projection_version: int = 0
    domain_revision: int | None = None
    lifecycle_generation: int = 0
    texts: dict = field(default_factory=dict)
    metadata: dict = field(default_factory=dict)
    vector_f32_le: bytes = b""
    vector_space: bytes = b""
    source_locator: bytes = b""
    source_revision: int | None = None
    payload: bytes = b""
    deleted: bool = False
    remove_metadata: tuple = ()
    replace_metadata: bool = False


def _integer(value):
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value < 2 ** 64:
        raise WireError("expected uint64")
    return value


def _varint(value):
    value = _integer(value)
    result = bytearray()
    while value >= 128:
        result.append((value & 127) | 128)
        value >>= 7
    result.append(value)
    return bytes(result)


def _u(number, value):
    return _varint(number << 3) + _varint(value)


def _b(number, value):
    if not isinstance(value, bytes) or len(value) > MAX_MESSAGE:
        raise WireError("expected bounded bytes")
    return _varint(number << 3 | 2) + _varint(len(value)) + value


def _utf8(value, maximum=MAX_MESSAGE):
    if not isinstance(value, str):
        raise WireError("expected UTF-8 string")
    encoded = value.encode("utf-8")
    if len(encoded) > maximum:
        raise WireError("UTF-8 string exceeds byte budget")
    return encoded


def _d(number, value):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise WireError("expected finite double")
    return _varint(number << 3 | 1) + struct.pack("<d", value)


def _bounded(result):
    if len(result) > MAX_MESSAGE:
        raise WireError("index message exceeds byte budget")
    return result


def scalar(value):
    if value is None:
        return _u(1, 0)
    if isinstance(value, bool):
        return _u(1, 5) + _u(6, int(value))
    if isinstance(value, UInt):
        return _u(1, 3) + _u(4, value.value)
    if isinstance(value, int):
        if not -(2 ** 63) <= value < 2 ** 63:
            raise WireError("signed metadata integer exceeds int64; use UInt for uint64")
        return _u(1, 2) + _u(3, (value << 1) ^ (value >> 63))
    if isinstance(value, float):
        return _u(1, 4) + _d(5, value)
    if isinstance(value, str):
        return _u(1, 1) + _b(2, _utf8(value, 65536))
    if isinstance(value, bytes) and len(value) <= 65536:
        return _u(1, 6) + _b(2, value)
    raise WireError("unsupported metadata scalar")


def vector(values):
    if isinstance(values, bytes):
        if len(values) % 4 or len(values) > 4096 * 4:
            raise WireError("invalid vector byte dimensions")
        decoded = struct.unpack("<" + "f" * (len(values) // 4), values)
        result = values
    else:
        decoded = tuple(values)
        if len(decoded) > 4096:
            raise WireError("vector exceeds 4096 dimensions")
        try:
            result = struct.pack("<" + "f" * len(decoded), *decoded)
        except (ValueError, TypeError, OverflowError, struct.error) as exc:
            raise WireError("invalid FP32 vector") from exc
    if any(not math.isfinite(value) for value in decoded) or (decoded and not any(decoded)):
        raise WireError("vector must be finite and nonzero")
    # FP64 values can overflow while packing FP32 even when originally finite.
    if any(not math.isfinite(value) for value in struct.unpack("<" + "f" * (len(result) // 4), result)):
        raise WireError("vector exceeds finite FP32 range")
    return result


def encode_document(document):
    if not isinstance(document, Document) or not isinstance(document.id, bytes) or not 1 <= len(document.id) <= 1024:
        raise WireError("document requires a 1..1024 byte id")
    if len(document.texts) > 32 or len(document.metadata) > 64 or len(document.remove_metadata) > 64:
        raise WireError("document field count exceeds v1 limits")
    result = _b(1, document.id) + _u(2, document.projection_version) + _u(4, document.lifecycle_generation)
    if document.domain_revision is not None:
        result += _u(3, document.domain_revision)
    for name, value in sorted(document.texts.items()):
        body, weight = value if isinstance(value, tuple) else (value, 1.0)
        encoded_name = _utf8(name, 128)
        if not encoded_name or not math.isfinite(weight) or not 0 < weight <= 1000000:
            raise WireError("invalid weighted text field")
        result += _b(5, _b(1, encoded_name) + _b(2, _utf8(body)) + _d(3, weight))
    for name, value in sorted(document.metadata.items()):
        encoded_name = _utf8(name, 128)
        if not encoded_name:
            raise WireError("empty metadata name")
        result += _b(6, _b(1, encoded_name) + _b(2, scalar(value)))
    if document.vector_f32_le:
        result += _b(7, vector(document.vector_f32_le))
    for number, value, maximum in ((8, document.vector_space, 512), (9, document.source_locator, 65536),
                                   (11, document.payload, 1048576)):
        if not isinstance(value, bytes) or len(value) > maximum:
            raise WireError("document bytes exceed v1 limits")
        if value:
            result += _b(number, value)
    if document.source_revision is not None:
        result += _u(10, document.source_revision)
    if type(document.deleted) is not bool or type(document.replace_metadata) is not bool:
        raise WireError("invalid document boolean")
    if document.deleted:
        result += _u(12, 1)
    for name in document.remove_metadata:
        result += _b(13, _utf8(name, 128))
    if document.replace_metadata:
        result += _u(14, 1)
    return _bounded(result)


def encode_predicate(predicate, _depth=0, _count=None):
    count = [0] if _count is None else _count
    count[0] += 1
    if not isinstance(predicate, Predicate) or _depth > 24 or count[0] > 256:
        raise WireError("predicate exceeds v1 nesting/node limits")
    op = predicate.operation
    if type(op) is not int or not 0 <= op <= 11 or len(predicate.values) > 256:
        raise WireError("invalid predicate operation/value count")
    if op >= 9:
        valid = not predicate.field and not predicate.values and (op != 11 or len(predicate.children) == 1)
    else:
        valid = not predicate.children
        valid &= ((op == 0 and not predicate.field and not predicate.values) or
                  (1 <= op <= 6 and bool(predicate.field) and len(predicate.values) == 1) or
                  (op == 7 and bool(predicate.field) and bool(predicate.values)) or
                  (op == 8 and bool(predicate.field) and not predicate.values))
    if not valid:
        raise WireError("invalid predicate shape")
    result = _u(1, op)
    if predicate.field:
        result += _b(2, _utf8(predicate.field, 128))
    for value in predicate.values:
        result += _b(3, scalar(value))
    for child in predicate.children:
        result += _b(4, encode_predicate(child, _depth + 1, count))
    return _bounded(result)


def _instance(index_space, partition):
    if not isinstance(index_space, bytes) or not 1 <= len(index_space) <= 128:
        raise WireError("index_space requires 1..128 bytes")
    if not isinstance(partition, bytes) or not 1 <= len(partition) <= 512:
        raise WireError("partition requires 1..512 bytes")
    return _b(1, index_space) + _b(2, partition)


def encode_mutation(*, index_space, partition, operation, expected_projection_version=0,
                    document=None, id=b"", next_projection_version=0, lifecycle_generation=0):
    operation = Mutation(operation)
    result = _instance(index_space, partition) + _u(3, operation) + _u(4, expected_projection_version) + _u(25, 1)
    if document is not None:
        result += _b(5, encode_document(document))
    if id:
        result += _b(6, id)
    if next_projection_version:
        result += _u(7, next_projection_version)
    if lifecycle_generation:
        result += _u(8, lifecycle_generation)
    return _bounded(result)


def encode_query(*, index_space, partition, operation=Query.SEARCH, text="", limit=8, where=None, mode=0,
                 vector_f32_le=b"", vector_space=b"", posting_budget=12000, candidate_budget=1024,
                 scan_budget=100000, memory_budget=16 * 1024 * 1024, timeout_ms=0, match_all_terms=False,
                 id=b"", after_id=b"", min_similarity=None, allow_partial=False, expected_projection_version=0,
                 statistics_policy=0, expected_mutation_sequence=0):
    operation = Query(operation)
    if not isinstance(text, str) or not isinstance(vector_space, bytes) or len(vector_space) > 512:
        raise WireError("query text/vector-space type or size is invalid")
    if any(not isinstance(value, bytes) or len(value) > 1024 for value in (id, after_id)):
        raise WireError("query id/cursor must be bounded bytes")
    if type(limit) is not int or not 1 <= limit <= 4096 or mode not in (0, 1) or statistics_policy != 0:
        raise WireError("invalid query limit/mode or unsupported statistics policy")
    if type(match_all_terms) is not bool or type(allow_partial) is not bool:
        raise WireError("invalid query boolean")
    if operation == Query.SEARCH and mode == 1:
        if text or match_all_terms or not vector_f32_le or not vector_space:
            raise WireError("dense search requires a vector/space and does not accept lexical terms")
    elif mode != 0 or vector_f32_le or vector_space or min_similarity is not None:
        raise WireError("vector arguments apply only to dense search")
    if min_similarity is not None and (
            isinstance(min_similarity, bool) or not isinstance(min_similarity, (int, float)) or
            not math.isfinite(min_similarity) or not -1 <= min_similarity <= 1):
        raise WireError("min_similarity must be finite and within [-1,1]")
    result = _instance(index_space, partition) + _u(3, operation) + _u(5, limit) + _u(7, mode) + _u(25, 1)
    if text:
        result += _b(4, _utf8(text))
    if where is not None:
        result += _b(6, encode_predicate(where))
    if vector_f32_le:
        result += _b(8, vector(vector_f32_le))
    if vector_space:
        result += _b(9, vector_space)
    for number, value in ((10, posting_budget), (11, candidate_budget), (12, scan_budget), (13, memory_budget),
                          (14, timeout_ms), (20, expected_projection_version), (22, expected_mutation_sequence)):
        result += _u(number, value)
    if match_all_terms:
        result += _u(15, 1)
    if id:
        result += _b(16, id)
    if after_id:
        result += _b(17, after_id)
    if min_similarity is not None:
        result += _d(18, min_similarity)
    if allow_partial:
        result += _u(19, 1)
    return _bounded(result)


def _read_varint(data, offset):
    value = 0
    for shift in range(0, 70, 7):
        if offset >= len(data):
            raise WireError("truncated protobuf varint")
        byte = data[offset]
        offset += 1
        if shift == 63 and byte > 1:
            raise WireError("protobuf varint exceeds uint64")
        value |= (byte & 127) << shift
        if byte < 128:
            return value, offset
    raise WireError("invalid protobuf varint")


def fields(data):
    if not isinstance(data, bytes) or len(data) > MAX_MESSAGE:
        raise WireError("invalid protobuf message byte budget")
    offset = 0
    while offset < len(data):
        key, offset = _read_varint(data, offset)
        number, wire = key >> 3, key & 7
        if not 0 < number < 2 ** 29:
            raise WireError("invalid protobuf field number")
        if wire == 0:
            value, offset = _read_varint(data, offset)
        else:
            if wire == 2:
                size, offset = _read_varint(data, offset)
            elif wire in (1, 5):
                size = 8 if wire == 1 else 4
            else:
                raise WireError("unsupported protobuf wire type")
            if size > len(data) - offset:
                raise WireError("truncated protobuf field")
            value, offset = data[offset:offset + size], offset + size
        yield number, wire, value


def _known(wire, expected, value):
    if wire != expected:
        raise WireError("known protobuf field has wrong wire type")
    return value


def decode_scalar(data):
    values = {}
    schema = {1: 0, 2: 2, 3: 0, 4: 0, 5: 1, 6: 0}
    for number, wire, value in fields(data):
        if number in schema:
            values[number] = _known(wire, schema[number], value)
    kind = values.get(1, 0)
    allowed = {0: (), 1: (2,), 2: (3,), 3: (4,), 4: (5,), 5: (6,), 6: (2,)}
    if kind not in allowed or any(number != 1 and number not in allowed[kind] for number in values):
        raise WireError("scalar kind and value disagree")
    if kind == 0:
        return None
    if kind in (1, 6):
        value = values.get(2, b"")
        if len(value) > 65536:
            raise WireError("scalar exceeds v1 byte limit")
        return value.decode("utf-8") if kind == 1 else value
    if kind == 2:
        value = values.get(3, 0)
        return (value >> 1) ^ -(value & 1)
    if kind == 3:
        return UInt(values.get(4, 0))
    if kind == 4:
        value = struct.unpack("<d", values.get(5, b"\0" * 8))[0]
        if not math.isfinite(value):
            raise WireError("non-finite result scalar")
        return value
    value = values.get(6, 0)
    if value > 1:
        raise WireError("invalid protobuf boolean")
    return bool(value)


def decode_document(data):
    document = {"id": b"", "projection_version": 0, "domain_revision": 0, "lifecycle_generation": 0,
                "texts": {}, "metadata": {}, "vector_f32_le": b"", "vector_space": b"", "source_locator": b"",
                "source_revision": 0, "payload": b"", "deleted": False}
    integers = {2: "projection_version", 3: "domain_revision", 4: "lifecycle_generation", 10: "source_revision"}
    binary = {1: "id", 7: "vector_f32_le", 8: "vector_space", 9: "source_locator", 11: "payload"}
    for number, wire, value in fields(data):
        if number in integers:
            document[integers[number]] = _known(wire, 0, value)
        elif number in binary:
            document[binary[number]] = _known(wire, 2, value)
        elif number in (5, 6):
            message = _known(wire, 2, value)
            name, body, weight = "", b"", 1.0
            for part, kind, content in fields(message):
                if part in (1, 2):
                    content = _known(kind, 2, content)
                    if part == 1:
                        name = content.decode("utf-8")
                    else:
                        body = content
                elif part == 3 and number == 5:
                    weight = struct.unpack("<d", _known(kind, 1, content))[0]
            target = document["texts" if number == 5 else "metadata"]
            if not name or name in target or len(name.encode()) > 128:
                raise WireError("invalid or duplicate result field name")
            if number == 5:
                if not math.isfinite(weight) or not 0 < weight <= 1000000 or len(target) >= 32:
                    raise WireError("invalid result text weight/count")
                target[name] = (body.decode("utf-8"), weight)
            else:
                if len(target) >= 64:
                    raise WireError("invalid result metadata count")
                target[name] = decode_scalar(body)
        elif number == 12:
            value = _known(wire, 0, value)
            if value > 1:
                raise WireError("invalid result deletion flag")
            document["deleted"] = bool(value)
        elif number in (13, 14):
            _known(wire, 2 if number == 13 else 0, value)
    if (len(document["id"]) > 1024 or len(document["vector_space"]) > 512 or
            len(document["source_locator"]) > 65536 or len(document["payload"]) > 1048576):
        raise WireError("result document exceeds v1 byte limits")
    if document["vector_f32_le"]:
        vector(document["vector_f32_le"])
    return document


def decode_result(data):
    result = {"hits": [], "snapshot_version": 0, "mutation_sequence": 0, "ranking_kind": 0, "termination": 0,
              "index_coverage": 0, "match_evidence": 0, "postings_visited": 0, "candidates": 0,
              "records_scanned": 0, "next_cursor": b"", "total_hits": None, "record_count": 0,
              "vector_count": 0, "strategy": "", "analyzer": ""}
    numbers = {2: "snapshot_version", 3: "mutation_sequence", 4: "ranking_kind", 5: "termination",
               6: "index_coverage", 7: "match_evidence", 8: "postings_visited", 9: "candidates",
               10: "records_scanned", 12: "total_hits", 13: "record_count", 14: "vector_count"}
    for number, wire, value in fields(data):
        if number in numbers:
            result[numbers[number]] = _known(wire, 0, value)
        elif number == 1:
            hit = {"document": None, "score": 0.0, "dense_similarity": None, "matched_fields": []}
            for part, kind, content in fields(_known(wire, 2, value)):
                if part == 1:
                    hit["document"] = decode_document(_known(kind, 2, content))
                elif part in (2, 3):
                    score = struct.unpack("<d", _known(kind, 1, content))[0]
                    if not math.isfinite(score):
                        raise WireError("non-finite result score")
                    hit["score" if part == 2 else "dense_similarity"] = score
                elif part == 4:
                    hit["matched_fields"].append(_known(kind, 2, content).decode("utf-8"))
            if hit["document"] is None or len(result["hits"]) >= 4096:
                raise WireError("missing document or oversized hit count")
            result["hits"].append(hit)
        elif number in (11, 15, 16):
            value = _known(wire, 2, value)
            result[{11: "next_cursor", 15: "strategy", 16: "analyzer"}[number]] = value if number == 11 else value.decode("utf-8")
    return result
