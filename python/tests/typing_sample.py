"""What user code looks like to a type checker: `mypy --strict tests/typing_sample.py` passes.

(Not a unittest: run it with the checker; `python tests/typing_sample.py` also runs it.)
"""

from __future__ import annotations

import enum
import os
import sys
from dataclasses import dataclass, field
from typing import Annotated, Dict, List, NewType, Optional, Protocol

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"))

import psio  # noqa: E402
from psio import u8, u32, u64  # noqa: E402

AccountId = NewType("AccountId", u64)


@psio.enum(u8)
class Color(enum.Enum):
    RED = 0
    GREEN = 1


@psio.record(max_dynamic_data=1024)
@dataclass
class Account:
    id: AccountId
    owner: Annotated[str, psio.max(32)]
    tags: Annotated[List[str], psio.max(8), psio.element(psio.max(16))]
    balance: u64 = 0
    color: Color = Color.RED
    limits: Dict[str, u32] = field(default_factory=dict)
    email: Annotated[Optional[str], psio.max(64)] = None

    @psio.invariant
    def _check(self) -> Optional[str]:
        return None if self.balance < 1 << 60 else "Account: balance too large"


@psio.interface(name="bank")
class Bank(Protocol):
    @psio.method(max_dynamic_data=256)
    def transfer(self, to: AccountId, amount: u64, memo: Annotated[str, psio.max(80)]) -> None: ...

    def balance(self, of: AccountId) -> u64: ...


def main() -> None:
    account = Account(AccountId(7), "alice", ["vip"])
    total: int = account.balance + 1           # a u64 field is an int
    data: bytes = psio.to_pssz(account)
    back: Account = psio.from_pssz(Account, data)
    status: psio.Status = psio.validate_pssz(Account, data)
    text: str = psio.to_ijson(back)
    again: Account = psio.from_ijson(Account, text)
    key: bytes = psio.to_key(account)
    size: int = psio.pssz_size(account)
    numbers: List[int] = psio.from_pssz(List[u32], psio.to_pssz([1, 2], List[u32]))
    schema: psio.Schema = psio.schema_of(Account)
    width: int = psio.codec(Account).width
    bank: psio.Interface = psio.interface_of(Bank)
    call: bytes = bank["transfer"].to_pssz(AccountId(1), 5, "rent")
    try:
        psio.from_pssz(Account, b"\xff")
    except psio.DecodeError as e:
        reason: str = e.reason
        assert reason
    assert again == account and status.ok and total == 1 and key and size == len(data) and numbers == [1, 2]
    assert schema.names()[0] == "Account" and width == 2 and len(call) == 2 + 8 + 8 + 2 + 4


if __name__ == "__main__":
    main()
