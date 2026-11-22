//! Deployment-time schema checks for the optional-suffix evolution contract.
//! Readers do not need the writer schema; old/new schemas are compared only
//! when an upgrade is admitted. Width must stay fixed within the lineage.
use crate::pssz::{PsszError, PsszResult};
use crate::pssz_schema_ir::{AnyType, AnyValue, Attribute, Member, Schema};
use serde::{Deserialize, Serialize};
use std::collections::HashSet;

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct UpgradeReport {
    pub added_optional_fields: Vec<String>,
}

pub fn validate_optional_upgrade(
    old: &Schema,
    old_type: &AnyType,
    new: &Schema,
    new_type: &AnyType,
) -> PsszResult<UpgradeReport> {
    old.check_ir_version()?;
    new.check_ir_version()?;
    references(old, old_type)?;
    references(new, new_type)?;
    // Both sides are well-formed schemas for the codecs they describe.
    crate::pssz_dynamic::metadata(old, old_type, true)?;
    crate::pssz_dynamic::metadata(new, new_type, true)?;
    let mut checker = Checker {
        old,
        new,
        visited: HashSet::new(),
        added: vec![],
    };
    checker.compare(old_type, new_type, "$", 0)?;
    Ok(UpgradeReport {
        added_optional_fields: checker.added,
    })
}

fn references(schema: &Schema, root: &AnyType) -> PsszResult<()> {
    let mut visited = HashSet::new();
    let mut pending = vec![root];
    while let Some(ty) = pending.pop() {
        let ty = schema.wire(ty)?;
        if !visited.insert(ty as *const AnyType as usize) {
            continue;
        }
        match &ty.value {
            AnyValue::Struct(s) => pending.extend(s.members.iter().map(|m| m.r#type.as_ref())),
            AnyValue::Object(o) => pending.extend(o.members.iter().map(|m| m.r#type.as_ref())),
            AnyValue::Variant(v) => pending.extend(v.members.iter().map(|m| m.r#type.as_ref())),
            AnyValue::Tuple(t) => pending.extend(t.members.iter().map(|m| m.as_ref())),
            AnyValue::Array(a) => pending.push(&a.r#type),
            AnyValue::List(l) => pending.push(&l.r#type),
            AnyValue::BoundedList(l) => pending.push(&l.r#type),
            AnyValue::Option(o) => pending.push(&o.r#type),
            AnyValue::Custom(c) => pending.push(&c.r#type),
            AnyValue::Nested(n) => pending.push(&n.r#type),
            AnyValue::Resource(_) => {
                return Err(PsszError::new(
                    "pssz upgrade: resource values require a registered adapter",
                ))
            }
            _ => (),
        }
    }
    Ok(())
}

struct Checker<'a> {
    old: &'a Schema,
    new: &'a Schema,
    visited: HashSet<(usize, usize)>,
    added: Vec<String>,
}
fn stable(attributes: &[Attribute]) -> bool {
    attributes
        .iter()
        .any(|a| a.name == "definitionWillNotChange")
}
impl Checker<'_> {
    fn compare(
        &mut self,
        old: &AnyType,
        new: &AnyType,
        path: &str,
        depth: usize,
    ) -> PsszResult<()> {
        if depth > 128 {
            return Err(PsszError::new("pssz upgrade: schema comparison depth exceeded"));
        }
        let old = self.old.wire(old)?;
        let new = self.new.wire(new)?;
        if !self.visited.insert((
            old as *const AnyType as usize,
            new as *const AnyType as usize,
        )) {
            return Ok(());
        }
        match (&old.value, &new.value) {
            (AnyValue::Object(a), AnyValue::Object(b)) if a.attributes == b.attributes => self
                .members(
                    &a.members,
                    &b.members,
                    !stable(&a.attributes),
                    path,
                    depth + 1,
                ),
            (AnyValue::Struct(a), AnyValue::Struct(b)) if a.attributes == b.attributes => {
                self.members(&a.members, &b.members, false, path, depth + 1)
            }
            (AnyValue::Tuple(a), AnyValue::Tuple(b)) => {
                if b.members.len() < a.members.len() {
                    return Err(PsszError::new("pssz upgrade: tuple fields removed"));
                }
                for (i, (old, new)) in a.members.iter().zip(&b.members).enumerate() {
                    self.compare(old, new, &format!("{path}.{i}"), depth + 1)?;
                }
                for (i, ty) in b.members.iter().enumerate().skip(a.members.len()) {
                    if !matches!(self.new.wire(ty)?.value, AnyValue::Option(_)) {
                        return Err(PsszError::new(
                            "pssz upgrade: appended tuple field is not optional",
                        ));
                    }
                    self.added.push(format!("{path}.{i}"));
                }
                Ok(())
            }
            (AnyValue::Array(a), AnyValue::Array(b)) if a.len == b.len => {
                self.compare(&a.r#type, &b.r#type, path, depth + 1)
            }
            (AnyValue::List(a), AnyValue::List(b)) => {
                self.compare(&a.r#type, &b.r#type, path, depth + 1)
            }
            (AnyValue::BoundedList(a), AnyValue::BoundedList(b)) if a.maxCount == b.maxCount => {
                self.compare(&a.r#type, &b.r#type, path, depth + 1)
            }
            (AnyValue::Option(a), AnyValue::Option(b)) => {
                self.compare(&a.r#type, &b.r#type, path, depth + 1)
            }
            (AnyValue::Custom(a), AnyValue::Custom(b)) if a.id == b.id => {
                self.compare(&a.r#type, &b.r#type, path, depth + 1)
            }
            // Nested(A) -> Nested(B) is compatible exactly when A -> B is.
            (AnyValue::Nested(a), AnyValue::Nested(b)) => {
                self.compare(&a.r#type, &b.r#type, path, depth + 1)
            }
            (AnyValue::Variant(a), AnyValue::Variant(b))
                if a.attributes == b.attributes && a.members.len() == b.members.len() =>
            {
                self.members(&a.members, &b.members, false, path, depth + 1)
            }
            (AnyValue::Int(a), AnyValue::Int(b)) if a == b => Ok(()),
            (AnyValue::Float(a), AnyValue::Float(b)) if a == b => Ok(()),
            _ => Err(PsszError::new(
                "pssz upgrade: existing field type, bounds, attributes, or variant changed",
            )),
        }
    }
    fn members(
        &mut self,
        old: &[Member],
        new: &[Member],
        extend: bool,
        path: &str,
        depth: usize,
    ) -> PsszResult<()> {
        if new.len() < old.len() || (!extend && new.len() != old.len()) {
            return Err(PsszError::new(
                "pssz upgrade: fields removed or frozen layout extended",
            ));
        }
        if old
            .iter()
            .enumerate()
            .any(|(i, m)| old[..i].iter().any(|p| p.name == m.name))
            || new
                .iter()
                .enumerate()
                .any(|(i, m)| new[..i].iter().any(|p| p.name == m.name))
        {
            return Err(PsszError::new("pssz upgrade: duplicate member names"));
        }
        for (old, new) in old.iter().zip(new) {
            if old.name != new.name || old.attributes != new.attributes {
                return Err(PsszError::new(
                    "pssz upgrade: existing member renamed, reordered, or reannotated",
                ));
            }
            self.compare(
                &old.r#type,
                &new.r#type,
                &format!("{path}.{}", old.name),
                depth + 1,
            )?;
        }
        for member in new.iter().skip(old.len()) {
            if !matches!(self.new.wire(&member.r#type)?.value, AnyValue::Option(_)) {
                return Err(PsszError::new("pssz upgrade: appended field is not optional"));
            }
            self.added.push(format!("{path}.{}", member.name));
        }
        Ok(())
    }
}
