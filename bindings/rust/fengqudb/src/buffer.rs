//! 数据库分配的缓冲：直接在原生内存上读取，Drop 时 `fqdb_buffer_free`。

use fengqudb_sys as sys;

/// 拥有原生内存的缓冲（复制读取结果或查询响应）。
///
/// 字节只读；`Drop` 时恰好释放一次。可跨线程移动。
pub struct Buffer {
    raw: sys::fqdb_buffer,
}

// SAFETY: Buffer 独占其原生分配且只提供只读访问，释放可发生在任意线程
// （fqdb_buffer_free 不依赖线程）。
unsafe impl Send for Buffer {}
// SAFETY: 只有 &self 的只读方法，没有内部可变性。
unsafe impl Sync for Buffer {}

impl Buffer {
    /// 接管一个由原生调用填充成功的缓冲。
    ///
    /// # Safety
    /// `raw` 必须是刚由成功的 C 调用写入、尚未释放的缓冲。
    pub(crate) unsafe fn from_raw(raw: sys::fqdb_buffer) -> Buffer {
        Buffer { raw }
    }

    /// 字节视图，生命周期绑定到本缓冲。
    #[inline]
    pub fn as_bytes(&self) -> &[u8] {
        if self.raw.size == 0 || self.raw.data.is_null() {
            return &[];
        }
        // SAFETY: data 指向 size 个已初始化字节，由本对象独占，直到 Drop 才释放；
        // 返回的引用生命周期受 &self 约束。
        unsafe { std::slice::from_raw_parts(self.raw.data.cast::<u8>(), self.raw.size as usize) }
    }

    /// 字节数。
    pub fn len(&self) -> usize {
        self.raw.size as usize
    }

    /// 是否为空。
    pub fn is_empty(&self) -> bool {
        self.raw.size == 0
    }

    /// 复制为 `Vec<u8>`（需要逃逸时使用）。
    pub fn to_vec(&self) -> Vec<u8> {
        self.as_bytes().to_vec()
    }
}

impl AsRef<[u8]> for Buffer {
    fn as_ref(&self) -> &[u8] {
        self.as_bytes()
    }
}

impl std::fmt::Debug for Buffer {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "Buffer({} bytes)", self.len())
    }
}

impl Drop for Buffer {
    fn drop(&mut self) {
        // SAFETY: raw 来自成功的原生调用且只在此处释放一次；
        // 空缓冲 (data == NULL) 对 fqdb_buffer_free 是允许的。
        unsafe { sys::fqdb_buffer_free(&mut self.raw) }
    }
}
