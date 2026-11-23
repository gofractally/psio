"""Interfaces and methods: argument tuples, caps, dispatch, a call proxy, the Schema IR."""

from __future__ import annotations

import abc
import unittest
from typing import Annotated, List, Optional

import support  # noqa: F401

import psio
from psio import u32, u64


@psio.interface(name="token", package="psi:token", version="1.0.0")
class Token(abc.ABC):
    @psio.method(max_dynamic_data=256)
    @abc.abstractmethod
    def transfer(self, to: Annotated[str, psio.max(16)], amount: u64,
                 memo: Annotated[str, psio.max(80)]) -> None: ...

    @abc.abstractmethod
    def balance_of(self, owner: Annotated[str, psio.max(16)]) -> u64: ...

    @abc.abstractmethod
    def batch(self, names: Annotated[List[str], psio.max(4), psio.element(psio.max(8))]) -> List[u64]: ...

    @staticmethod
    def version() -> u32:
        return 1

    def _private(self) -> None:
        pass


class Ledger(Token):
    def __init__(self) -> None:
        self.balances: dict = {}

    def transfer(self, to: str, amount: int, memo: str) -> None:
        self.balances[to] = self.balances.get(to, 0) + amount

    def balance_of(self, owner: str) -> int:
        return self.balances.get(owner, 0)  # type: ignore[no-any-return]

    def batch(self, names: List[str]) -> List[int]:
        return [self.balance_of(n) for n in names]


class Interfaces(unittest.TestCase):
    def test_metadata(self) -> None:
        token = psio.interface_of(Token)
        self.assertEqual(token.name, "token")
        self.assertEqual([m.name for m in token], ["transfer", "balance_of", "batch", "version"])
        self.assertEqual(token["transfer"].params, ["to", "amount", "memo"])
        self.assertEqual(token.index_of("balance_of"), 1)
        self.assertIsNone(token.index_of("nope"))
        # transfer: an explicit cap rules (2-byte offsets); balance_of: the inferred bound (1-byte).
        self.assertEqual((token["transfer"].args.width, token["transfer"].args.max_dynamic_data), (2, 256))
        self.assertEqual((token["balance_of"].args.width, token["balance_of"].args.max_encoded_size), (1, 24))
        self.assertEqual(token["version"].params, [])

    def test_arguments_are_the_tuple_of_the_parameters(self) -> None:
        token = psio.interface_of(Token)
        data = token["balance_of"].to_pssz("bob")
        self.assertEqual(data, psio.to_pssz(("bob",), Annotated[tuple[Annotated[str, psio.max(16)]], psio.identity("x")]))
        self.assertEqual(token["balance_of"].from_pssz(data), ("bob",))
        self.assertEqual(token["transfer"].to_pssz("bob", 5, memo="hi"), token["transfer"].to_pssz("bob", 5, "hi"))
        self.assertEqual(token["transfer"].args.to_json(("bob", 5, "hi")), '{"to":"bob","amount":5,"memo":"hi"}')
        self.assertEqual(token["transfer"].args.from_json('{"to":"bob","amount":5,"memo":"hi"}'), ("bob", 5, "hi"))
        for bad in (lambda: token["transfer"].to_pssz("x" * 17, 1, ""),
                    lambda: token["batch"].to_pssz(["a"] * 5),
                    lambda: token["batch"].to_pssz(["x" * 9]),
                    lambda: token["transfer"].to_pssz("bob", 5, "hi", "extra"),
                    lambda: token["transfer"].to_pssz("bob", 5, nope=1)):
            with self.assertRaises(psio.EncodeError):
                bad()
        # Arguments over the cap are refused by the reader too.
        frame = psio.codec(tuple[str, u64, str], width=2).to_pssz(("b", 1, "m" * 250))
        self.assertEqual(token["transfer"].args.validate_pssz(frame).reason,
                         "pssz: encoded size exceeds maxDynamicData cap")

    def test_dispatch_and_proxy(self) -> None:
        token = psio.interface_of(Token)
        ledger = Ledger()
        self.assertEqual(token.dispatch(ledger, 0, token["transfer"].to_pssz("bob", 5, "hi")), b"")
        reply = token.dispatch(ledger, "balance_of", token["balance_of"].to_pssz("bob"))
        self.assertEqual(psio.from_pssz(u64, reply), 5)
        self.assertEqual(token.dispatch(ledger, "version", token["version"].to_pssz()), (1).to_bytes(4, "little"))
        with self.assertRaises(psio.DecodeError):
            token.dispatch(ledger, 0, b"\xff\xff\xff")
        with self.assertRaises(psio.PsioError):
            token.dispatch(ledger, "nope", b"")
        calls = []

        def send(method: psio.Method, data: bytes) -> bytes:
            calls.append(method.name)
            return token.dispatch(ledger, method.index, data)
        remote = token.proxy(send)
        remote.transfer("alice", 7, memo="x")
        self.assertEqual(remote.balance_of("alice"), 7)
        self.assertEqual(remote.batch(["alice", "bob"]), [7, 5])
        self.assertEqual(remote.version(), 1)
        self.assertEqual(calls, ["transfer", "balance_of", "batch", "version"])

    def test_schema(self) -> None:
        schema = psio.interface_of(Token).schema()
        iface = schema.ir["interfaces"][0]
        self.assertEqual(iface["name"], "token")
        self.assertEqual(schema.ir["package"], {"name": "psi:token", "version": "1.0.0", "attributes": []})
        self.assertEqual([f["name"] for f in iface["funcs"]], ["transfer", "balance_of", "batch", "version"])
        self.assertEqual(iface["funcs"][0]["attributes"], [{"name": "maxDynamicData", "value": "256"}])
        self.assertIsNone(iface["funcs"][0]["result"])
        self.assertEqual([p["name"] for p in iface["funcs"][0]["params"]], ["to", "amount", "memo"])
        # The schema is a psio::Schema value: it travels as bytes and as text.
        self.assertEqual(psio.Schema.from_pssz(schema.to_pssz()).ir, schema.ir)
        self.assertEqual(psio.Schema.from_json(schema.to_json()).to_pssz(), schema.to_pssz())

    def test_schema_driven_calls(self) -> None:
        """A tool that has only the schema (as bytes) encodes the same calls, from plain values or from text."""
        token = psio.interface_of(Token)
        fetched = psio.Schema.from_pssz(token.schema().to_pssz())
        transfer = fetched.method("token", "transfer")
        self.assertEqual(transfer.params, ["to", "amount", "memo"])
        self.assertEqual((transfer.args.width, transfer.args.max_dynamic_data), (2, 256))
        self.assertIsNone(transfer.result)
        typed = token["transfer"].to_pssz("bob", 5, "hi")
        self.assertEqual(transfer.args.to_pssz({"to": "bob", "amount": 5, "memo": "hi"}), typed)
        self.assertEqual(transfer.args.json_to_pssz('{"to":"bob","amount":5,"memo":"hi"}'), typed)
        self.assertEqual(transfer.args.from_pssz(typed), {"to": "bob", "amount": 5, "memo": "hi"})
        balance = fetched.method("token", "balance_of")
        self.assertEqual(balance.result.from_pssz((5).to_bytes(8, "little")), 5)
        with self.assertRaises(psio.SchemaError):
            fetched.method("token", "nope")

    def test_evolution_under_the_cap(self) -> None:
        @psio.interface
        class V2:
            @psio.method(max_dynamic_data=256, name="transfer")
            def transfer_(self, to: Annotated[str, psio.max(16)], amount: u64, memo: Annotated[str, psio.max(80)],
                          fee: Optional[u64] = None) -> None: ...

        old = psio.interface_of(Token)["transfer"]
        new = psio.interface_of(V2)["transfer"]
        data = old.to_pssz("bob", 5, "hi")
        self.assertEqual(new.to_pssz("bob", 5, "hi"), data)
        self.assertEqual(new.from_pssz(data), ("bob", 5, "hi", None))
        with_fee = new.to_pssz("bob", 5, "hi", fee=1)
        st = old.args.validate_pssz(with_fee)
        self.assertTrue(st.ok and st.has_unknown)
        self.assertEqual(old.from_pssz(with_fee), ("bob", 5, "hi"))


if __name__ == "__main__":
    unittest.main()
