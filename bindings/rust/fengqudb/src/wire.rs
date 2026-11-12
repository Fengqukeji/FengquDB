//! protobuf 线格式的最小编码/解码工具（仅供查询模块使用，无 unsafe）。

use crate::error::{Error, Result};

const TRUNCATED: Error = Error::Decode("truncated feature response");
const INVALID: Error = Error::Decode("invalid feature response");

/// 追加 varint。
pub(crate) fn put_varint(out: &mut Vec<u8>, mut v: u64) {
    while v >= 128 {
        out.push((v as u8 & 0x7f) | 0x80);
        v >>= 7;
    }
    out.push(v as u8);
}

/// 追加 varint 字段（tag = number*8）。
pub(crate) fn put_uint(out: &mut Vec<u8>, number: u32, v: u64) {
    put_varint(out, u64::from(number) * 8);
    put_varint(out, v);
}

/// 追加 length-delimited 字段（tag = number*8+2）。
pub(crate) fn put_bytes(out: &mut Vec<u8>, number: u32, v: &[u8]) {
    put_varint(out, u64::from(number) * 8 + 2);
    put_varint(out, v.len() as u64);
    out.extend_from_slice(v);
}

/// 解码得到的字段值。
#[derive(Clone, Copy, Debug)]
pub(crate) enum Wire<'a> {
    Varint(u64),
    Fixed64(u64),
    /// 32 位定长字段：只校验长度，内容不被任何响应字段使用。
    Fixed32,
    Bytes(&'a [u8]),
}

impl Wire<'_> {
    /// 数值字段（varint 或 fixed64）。
    pub(crate) fn as_u64(&self) -> Option<u64> {
        match *self {
            Wire::Varint(v) | Wire::Fixed64(v) => Some(v),
            _ => None,
        }
    }
}

/// 逐字段迭代一条 protobuf 消息；不分配，字节字段借用输入。
///
/// 遇到截断或不支持的线类型时产出一次 `Err` 然后结束。
#[derive(Clone)]
pub(crate) struct Fields<'a> {
    data: &'a [u8],
    pos: usize,
}

impl<'a> Fields<'a> {
    pub(crate) fn new(data: &'a [u8]) -> Self {
        Fields { data, pos: 0 }
    }

    fn varint(&mut self) -> Result<u64> {
        let mut result = 0u64;
        let mut shift = 0u32;
        loop {
            let byte = *self.data.get(self.pos).ok_or(TRUNCATED)?;
            self.pos += 1;
            if shift == 63 && byte > 1 {
                return Err(INVALID);
            }
            result |= u64::from(byte & 0x7f) << shift;
            if byte < 0x80 {
                return Ok(result);
            }
            shift += 7;
            if shift > 63 {
                return Err(INVALID);
            }
        }
    }

    fn take(&mut self, n: usize) -> Result<&'a [u8]> {
        let end = self.pos.checked_add(n).ok_or(TRUNCATED)?;
        let slice = self.data.get(self.pos..end).ok_or(TRUNCATED)?;
        self.pos = end;
        Ok(slice)
    }

    fn next_field(&mut self) -> Result<(u32, Wire<'a>)> {
        let tag = self.varint()?;
        let number = (tag >> 3) as u32;
        let wire = match tag & 7 {
            0 => Wire::Varint(self.varint()?),
            1 => {
                let b = self.take(8)?;
                Wire::Fixed64(u64::from_le_bytes([b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]]))
            }
            2 => {
                let len = self.varint()?;
                let len = usize::try_from(len).map_err(|_| TRUNCATED)?;
                Wire::Bytes(self.take(len)?)
            }
            5 => {
                self.take(4)?;
                Wire::Fixed32
            }
            _ => return Err(INVALID),
        };
        Ok((number, wire))
    }
}

impl<'a> Iterator for Fields<'a> {
    type Item = Result<(u32, Wire<'a>)>;

    fn next(&mut self) -> Option<Self::Item> {
        if self.pos >= self.data.len() {
            return None;
        }
        let item = self.next_field();
        if item.is_err() {
            self.pos = self.data.len();
        }
        Some(item)
    }
}
