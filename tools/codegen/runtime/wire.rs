use std::fmt;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct FqModelError;
impl fmt::Display for FqModelError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result { f.write_str("invalid FengquDB model wire data, range, or size") }
}
impl std::error::Error for FqModelError {}
type FqResult<T> = Result<T, FqModelError>;
const FQ_LIMIT: usize = 16 * 1024 * 1024;
struct FqWriter<'a> { data: &'a mut Vec<u8>, start: usize }
impl<'a> FqWriter<'a> {
    fn raw(&mut self, b: &[u8]) -> FqResult<()> {
        if b.len() > FQ_LIMIT.saturating_sub(self.data.len()-self.start) { return Err(FqModelError); }
        self.data.extend_from_slice(b); Ok(())
    }
    fn u(&mut self, mut v: u64) -> FqResult<()> {
        let mut b=[0u8;10];let mut n=0;
        while v>=128 {b[n]=(v as u8 &127)|128;n+=1;v>>=7;}
        b[n]=v as u8;self.raw(&b[..n+1])
    }
    fn b(&mut self,b:&[u8])->FqResult<()> {self.u(b.len() as u64)?;self.raw(b)}
}
struct FqReader<'a> {data: &'a [u8],pos: usize}
impl<'a> FqReader<'a> {
    fn new(data:&'a [u8])->FqResult<Self> {if data.len()>FQ_LIMIT{return Err(FqModelError)};Ok(Self{data,pos:0})}
    fn u(&mut self)->FqResult<u64> {
        let mut v=0u64;
        for i in 0..10 {let b=*self.data.get(self.pos).ok_or(FqModelError)?;self.pos+=1;
            if i==9 && b>1{return Err(FqModelError)};v|=u64::from(b&127)<<(i*7);if b<128{return Ok(v)}
        } Err(FqModelError)
    }
    fn tag(&mut self)->FqResult<u32> {let t=self.u()?;if t>u32::MAX as u64 || t>>3==0 || t&7>5 {return Err(FqModelError)};Ok(t as u32)}
    fn take(&mut self,n:u64)->FqResult<&'a [u8]> {
        if n>(self.data.len()-self.pos) as u64{return Err(FqModelError)};
        let begin=self.pos;self.pos+=n as usize;Ok(&self.data[begin..self.pos])
    }
    fn b(&mut self)->FqResult<&'a [u8]> {let n=self.u()?;self.take(n)}
    fn fixed32(&mut self)->FqResult<u32>{let b=self.take(4)?;Ok(u32::from_le_bytes([b[0],b[1],b[2],b[3]]))}
    fn fixed64(&mut self)->FqResult<u64>{let b=self.take(8)?;Ok(u64::from_le_bytes([b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7]]))}
    fn skip(&mut self,t:u32,depth:u32)->FqResult<()> {
        match t&7 {
            0=>{self.u()?;},1=>{self.take(8)?;},2=>{self.b()?;},5=>{self.take(4)?;},
            3 if depth<64=>{while self.pos<self.data.len(){let inner=self.tag()?;if inner&7==4{return if inner>>3==t>>3 {Ok(())}else{Err(FqModelError)}};self.skip(inner,depth+1)?;}return Err(FqModelError)},
            _=>return Err(FqModelError)
        } Ok(())
    }
}
fn fq_z(v:i64)->u64 {((v as u64)<<1)^((v>>63) as u64)}
fn fq_unz(v:u64)->i64 {((v>>1) as i64)^-((v&1) as i64)}
fn fq_key(prefix:&str,id:&str)->FqResult<Vec<u8>> {
    if id.len()>65536-prefix.len(){return Err(FqModelError)};
    let mut out=Vec::with_capacity(prefix.len()+id.len());out.extend_from_slice(prefix.as_bytes());out.extend_from_slice(id.as_bytes());Ok(out)
}
