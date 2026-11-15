from dataclasses import dataclass

# fqdb:entity prefix=student/ key=id
@dataclass
class Student:
    id: int = 0
    name: str = ""
    score: int = 0


# Types can also be inferred from unambiguous literal defaults.
# fqdb:entity prefix=counter/ key=id
class Counter:
    id = 0
    title = ""
    enabled = True
    total = 0
