// fqdb:entity prefix=student/ key=id
#[derive(Default)]
pub struct Student {
    pub id: i64,
    pub name: String,
    pub score: i32,
}
