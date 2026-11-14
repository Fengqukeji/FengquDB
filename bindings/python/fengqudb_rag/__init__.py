"""Native-index RAG: memory governance, injectable Embedding and bounded sources."""
from .types import (Chunk, Namespace, QueryBudget, SourceTicket, RagError, RagConflict,
    RagNotFound, RagUnsupported, RagIntegrityError, RagStaleRead, RagCancelled,
    RagDeadline, RagLimit, RagRecoveryRequired)
from .native import NativeIndexProvider
from .sources import NativeRagStore
from .memory import MemoryStore, ScopedMemoryStore
from .models import MemoryWrite, MemoryImport, MemorySearch, MemoryList, MemoryDelete
from .embedding import EmbeddingContract, CallbackEmbedding
from .context import pack_context

__all__ = ["Chunk", "Namespace", "QueryBudget", "SourceTicket", "NativeIndexProvider", "NativeRagStore",
    "RagError", "RagConflict", "RagNotFound", "RagUnsupported", "RagIntegrityError", "RagStaleRead",
    "RagCancelled", "RagDeadline", "RagLimit", "RagRecoveryRequired", "MemoryStore", "ScopedMemoryStore",
    "MemoryWrite", "MemoryImport", "MemorySearch", "MemoryList", "MemoryDelete", "EmbeddingContract", "CallbackEmbedding", "pack_context"]
