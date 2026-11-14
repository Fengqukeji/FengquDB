"""Resolve sibling SDK bindings without creating process-global module aliases."""

if "." in __package__:
    # e.g. FengquDB.bindings.python.fengqudb_rag: use that SDK's wrappers.
    # A broken packaged dependency must propagate, never select another SDK.
    from ..fengqudb import Store, Error as StoreError
    from .. import fengqudb_index as ffi
    from ..fengqudb_index_wire import vector
else:
    # Standalone source layout: bindings/python is already on the host path.
    from fengqudb import Store, Error as StoreError
    import fengqudb_index as ffi
    from fengqudb_index_wire import vector
