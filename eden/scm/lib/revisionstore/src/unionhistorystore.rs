/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This software may be used and distributed according to the terms of the
 * GNU General Public License version 2.
 */

// Union history store
use anyhow::Result;

use types::{Key, NodeInfo};

use crate::{
    historystore::{HgIdHistoryStore, RemoteHistoryStore},
    unionstore::UnionStore,
};

pub type UnionHgIdHistoryStore<T> = UnionStore<T>;

impl<T: HgIdHistoryStore> HgIdHistoryStore for UnionHgIdHistoryStore<T> {
    fn get_node_info(&self, key: &Key) -> Result<Option<NodeInfo>> {
        for store in self {
            match store.get_node_info(key)? {
                None => continue,
                Some(res) => return Ok(Some(res)),
            }
        }

        Ok(None)
    }
}

impl<T: RemoteHistoryStore> RemoteHistoryStore for UnionHgIdHistoryStore<T> {
    fn prefetch(&self, keys: &[Key]) -> Result<()> {
        let initial_keys = Ok(keys.to_vec());
        self.into_iter()
            .fold(initial_keys, |missing_keys, store| match missing_keys {
                Ok(missing_keys) => {
                    if !missing_keys.is_empty() {
                        store.prefetch(&missing_keys)?;
                        store.get_missing(&missing_keys)
                    } else {
                        Ok(vec![])
                    }
                }
                Err(e) => Err(e),
            })?;

        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    use quickcheck::quickcheck;
    use thiserror::Error;

    use crate::localstore::HgIdLocalStore;

    struct BadHgIdHistoryStore;

    struct EmptyHgIdHistoryStore;

    #[derive(Debug, Error)]
    #[error("Bad history store always has error which is not KeyError")]
    struct BadHgIdHistoryStoreError;

    impl HgIdHistoryStore for EmptyHgIdHistoryStore {
        fn get_node_info(&self, _key: &Key) -> Result<Option<NodeInfo>> {
            Ok(None)
        }
    }

    impl HgIdLocalStore for EmptyHgIdHistoryStore {
        fn get_missing(&self, keys: &[Key]) -> Result<Vec<Key>> {
            Ok(keys.iter().cloned().collect())
        }
    }

    impl HgIdHistoryStore for BadHgIdHistoryStore {
        fn get_node_info(&self, _key: &Key) -> Result<Option<NodeInfo>> {
            Err(BadHgIdHistoryStoreError.into())
        }
    }

    impl HgIdLocalStore for BadHgIdHistoryStore {
        fn get_missing(&self, _keys: &[Key]) -> Result<Vec<Key>> {
            Err(BadHgIdHistoryStoreError.into())
        }
    }

    quickcheck! {
        fn test_empty_unionstore_get_node_info(key: Key) -> bool {
            match UnionHgIdHistoryStore::<EmptyHgIdHistoryStore>::new().get_node_info(&key) {
                Ok(None) => true,
                _ => false,
            }
        }

        fn test_empty_historystore_get_node_info(key: Key) -> bool {
            let mut unionstore = UnionHgIdHistoryStore::new();
            unionstore.add(EmptyHgIdHistoryStore);
            match unionstore.get_node_info(&key) {
                Ok(None) => true,
                _ => false,
            }
        }

        fn test_bad_historystore_get_node_info(key: Key) -> bool {
            let mut unionstore = UnionHgIdHistoryStore::new();
            unionstore.add(BadHgIdHistoryStore);
            match unionstore.get_node_info(&key) {
                Err(_) => true,
                _ => false,
            }
        }

        fn test_empty_unionstore_get_missing(keys: Vec<Key>) -> bool {
            keys == UnionHgIdHistoryStore::<EmptyHgIdHistoryStore>::new().get_missing(&keys).unwrap()
        }

        fn test_empty_historystore_get_missing(keys: Vec<Key>) -> bool {
            let mut unionstore = UnionHgIdHistoryStore::new();
            unionstore.add(EmptyHgIdHistoryStore);
            keys == unionstore.get_missing(&keys).unwrap()
        }

        fn test_bad_historystore_get_missing(keys: Vec<Key>) -> bool {
            let mut unionstore = UnionHgIdHistoryStore::new();
            unionstore.add(BadHgIdHistoryStore);
            match unionstore.get_missing(&keys) {
                Err(_) => true,
                _ => false,
            }
        }
    }
}
