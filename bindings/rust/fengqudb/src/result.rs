//! 查询响应：直接在原生 `fqdb_buffer` 上惰性解码。
//!
//! `QueryResult` 拥有原生缓冲；行、值、访问计划都是借用它的迭代器，
//! 只在被访问时解码，不会先整体复制到 Rust 堆。

use crate::buffer::Buffer;
use crate::error::{Error, Result};
use crate::wire::{Fields, Wire};

const BAD_KIND: Error = Error::Decode("invalid query value kind");
const BAD_SECTION: Error = Error::Decode("invalid feature response");

/// 响应中解码出的值（借用响应缓冲）。
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Value<'a> {
    Null,
    Uint(u64),
    Int(i64),
    Double(f64),
    Bytes(&'a [u8]),
    Bool(bool),
}

impl<'a> Value<'a> {
    pub fn is_null(&self) -> bool {
        matches!(self, Value::Null)
    }

    /// 无符号或非负整数。
    pub fn as_u64(&self) -> Option<u64> {
        match *self {
            Value::Uint(v) => Some(v),
            Value::Int(v) => u64::try_from(v).ok(),
            _ => None,
        }
    }

    /// 有符号整数（无符号值需落在 i64 范围）。
    pub fn as_i64(&self) -> Option<i64> {
        match *self {
            Value::Int(v) => Some(v),
            Value::Uint(v) => i64::try_from(v).ok(),
            _ => None,
        }
    }

    pub fn as_f64(&self) -> Option<f64> {
        match *self {
            Value::Double(v) => Some(v),
            _ => None,
        }
    }

    pub fn as_bytes(&self) -> Option<&'a [u8]> {
        match *self {
            Value::Bytes(v) => Some(v),
            _ => None,
        }
    }

    pub fn as_bool(&self) -> Option<bool> {
        match *self {
            Value::Bool(v) => Some(v),
            _ => None,
        }
    }
}

/// 解码一个 `QueryValue` 消息，规则同 Python `_value_at`。
fn decode_value(data: &[u8]) -> Result<Value<'_>> {
    let (mut kind, mut u, mut i, mut real_bits, mut raw) = (0u64, 0u64, 0u64, 0u64, &[][..]);
    for field in Fields::new(data) {
        match field? {
            (1, Wire::Varint(x)) => kind = x,
            (2, Wire::Varint(x)) => u = x,
            (3, Wire::Varint(x)) => i = x,
            (4, Wire::Fixed64(x)) => real_bits = x,
            (5, Wire::Bytes(b)) => raw = b,
            _ => {}
        }
    }
    Ok(match kind {
        0 => Value::Null,
        1 => Value::Uint(u),
        2 => Value::Int(((i >> 1) as i64) ^ -((i & 1) as i64)),
        3 => Value::Double(f64::from_bits(real_bits)),
        4 => Value::Bytes(raw),
        5 => Value::Bool(u != 0),
        _ => return Err(BAD_KIND),
    })
}

/// 一次查询（或 EXPLAIN）的响应，拥有原生缓冲。
#[derive(Debug)]
pub struct QueryResult {
    buffer: Buffer,
    /// 查询响应在外层 feature response（字段 1）中的字节范围。
    start: usize,
    len: usize,
}

/// 统计信息（响应字段 3）。
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Stats {
    pub candidates: u64,
    pub matched: u64,
    pub result_total: u64,
    pub work: u64,
    pub peak_memory: u64,
    pub payload_reads: u64,
    pub join_probes: u64,
    pub complete: bool,
}

/// 访问计划的一项（响应字段 2），字符串借用响应缓冲。
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Access<'a> {
    pub source: u64,
    pub strategy: &'a str,
    pub index: &'a str,
    pub reason: &'a str,
    pub covering: bool,
    pub ordered: bool,
    pub needs_scan: bool,
    pub sampled_candidates: u64,
}

impl QueryResult {
    /// 由原生响应缓冲构造：先定位外层 feature response 的字段 1（查询响应），不复制。
    pub(crate) fn new(buffer: Buffer) -> Result<QueryResult> {
        let outer = buffer.as_bytes();
        let mut found: Option<&[u8]> = None;
        for field in Fields::new(outer) {
            if let (1, Wire::Bytes(b)) = field? {
                found = Some(b);
            }
        }
        let body = found.ok_or(Error::Decode("missing query response"))?;
        // body 是 outer 的子切片，偏移由指针差得到（纯安全代码）。
        let start = body.as_ptr() as usize - outer.as_ptr() as usize;
        let len = body.len();
        Ok(QueryResult { buffer, start, len })
    }

    fn body(&self) -> &[u8] {
        &self.buffer.as_bytes()[self.start..self.start + self.len]
    }

    /// 查询响应字节（外层 feature response 的字段 1）。
    pub fn as_bytes(&self) -> &[u8] {
        self.body()
    }

    /// 迭代结果行；行与值按需解码。
    pub fn rows(&self) -> ResultRows<'_> {
        ResultRows { fields: Fields::new(self.body()) }
    }

    /// 续页游标；为空表示没有更多页。
    pub fn cursor(&self) -> Result<&[u8]> {
        let mut cursor: &[u8] = &[];
        for field in Fields::new(self.body()) {
            if let (4, Wire::Bytes(b)) = field? {
                cursor = b;
            }
        }
        Ok(cursor)
    }

    /// 统计信息；响应没有该段时全为 0。
    pub fn stats(&self) -> Result<Stats> {
        let mut stats = Stats::default();
        for field in Fields::new(self.body()) {
            if let (3, Wire::Bytes(section)) = field? {
                stats = Stats::default();
                for item in Fields::new(section) {
                    let (n, wire) = item?;
                    let Some(x) = wire.as_u64() else { continue };
                    match n {
                        1 => stats.candidates = x,
                        2 => stats.matched = x,
                        3 => stats.result_total = x,
                        4 => stats.work = x,
                        5 => stats.peak_memory = x,
                        6 => stats.payload_reads = x,
                        7 => stats.join_probes = x,
                        8 => stats.complete = x != 0,
                        _ => {}
                    }
                }
            }
        }
        Ok(stats)
    }

    /// 迭代访问计划（EXPLAIN 与 SELECT 都会返回）。
    pub fn access(&self) -> AccessIter<'_> {
        AccessIter { fields: Fields::new(self.body()) }
    }
}

/// 结果行迭代器。
pub struct ResultRows<'a> {
    fields: Fields<'a>,
}

impl<'a> Iterator for ResultRows<'a> {
    type Item = Result<ResultRow<'a>>;

    fn next(&mut self) -> Option<Self::Item> {
        loop {
            match self.fields.next()? {
                Err(e) => return Some(Err(e)),
                Ok((1, Wire::Bytes(body))) => return Some(ResultRow::parse(body)),
                Ok(_) => {}
            }
        }
    }
}

/// 一行：键 + 按需解码的值。
#[derive(Clone, Copy, Debug)]
pub struct ResultRow<'a> {
    key: &'a [u8],
    body: &'a [u8],
}

impl<'a> ResultRow<'a> {
    /// 校验行内帧结构并定位键（不解码值）。
    fn parse(body: &'a [u8]) -> Result<ResultRow<'a>> {
        let mut key: &[u8] = &[];
        for field in Fields::new(body) {
            if let (1, Wire::Bytes(b)) = field? {
                key = b;
            }
        }
        Ok(ResultRow { key, body })
    }

    /// 记录键。
    pub fn key(&self) -> &'a [u8] {
        self.key
    }

    /// 按需解码各个值。
    pub fn values(&self) -> ResultValues<'a> {
        ResultValues { fields: Fields::new(self.body) }
    }

    /// 第 `index` 个值；越界返回 `Ok(None)`。
    pub fn value(&self, index: usize) -> Result<Option<Value<'a>>> {
        self.values().nth(index).transpose()
    }
}

/// 行内值迭代器。
pub struct ResultValues<'a> {
    fields: Fields<'a>,
}

impl<'a> Iterator for ResultValues<'a> {
    type Item = Result<Value<'a>>;

    fn next(&mut self) -> Option<Self::Item> {
        loop {
            match self.fields.next()? {
                Err(e) => return Some(Err(e)),
                Ok((2, Wire::Bytes(b))) => return Some(decode_value(b)),
                Ok(_) => {}
            }
        }
    }
}

/// 访问计划迭代器。
pub struct AccessIter<'a> {
    fields: Fields<'a>,
}

fn text(bytes: &[u8]) -> Result<&str> {
    std::str::from_utf8(bytes).map_err(|_| BAD_SECTION)
}

fn parse_access(section: &[u8]) -> Result<Access<'_>> {
    let mut a = Access::default();
    for item in Fields::new(section) {
        let (n, wire) = item?;
        match (n, wire) {
            (2, Wire::Bytes(b)) => a.strategy = text(b)?,
            (3, Wire::Bytes(b)) => a.index = text(b)?,
            (4, Wire::Bytes(b)) => a.reason = text(b)?,
            (1, w) => a.source = w.as_u64().unwrap_or(0),
            (5, w) => a.covering = w.as_u64().unwrap_or(0) != 0,
            (6, w) => a.ordered = w.as_u64().unwrap_or(0) != 0,
            (7, w) => a.needs_scan = w.as_u64().unwrap_or(0) != 0,
            (8, w) => a.sampled_candidates = w.as_u64().unwrap_or(0),
            _ => {}
        }
    }
    Ok(a)
}

impl<'a> Iterator for AccessIter<'a> {
    type Item = Result<Access<'a>>;

    fn next(&mut self) -> Option<Self::Item> {
        loop {
            match self.fields.next()? {
                Err(e) => return Some(Err(e)),
                Ok((2, Wire::Bytes(b))) => return Some(parse_access(b)),
                Ok(_) => {}
            }
        }
    }
}
