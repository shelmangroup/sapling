// Copyright (c) 2018-present, Facebook, Inc.
// All Rights Reserved.
//
// This software may be used and distributed according to the terms of the
// GNU General Public License version 2 or any later version.

#![deny(warnings)]
extern crate bytes;
extern crate chashmap;
#[macro_use]
extern crate cloned;
#[macro_use]
extern crate failure_ext as failure;
extern crate futures;
extern crate futures_ext;

extern crate blobrepo;
extern crate mercurial_types;
extern crate mononoke_types;

extern crate rust_thrift;
extern crate skiplist_thrift;

mod helpers;

pub mod errors;
pub use errors::ErrorKind;

mod index;
pub use index::{LeastCommonAncestorsHint, NodeFrontier, ReachabilityIndex, SimpleLcaHint};

mod genbfs;
pub use genbfs::GenerationNumberBFS;

mod skiplist;
pub use skiplist::{SkiplistIndex, SkiplistNodeType};
#[cfg(test)]
pub extern crate async_unit;
#[cfg(test)]
pub extern crate fixtures;
#[cfg(test)]
mod tests;
