"""Explicit integration factory. This file does not change or notify WordGame."""
import hashlib


def build_wordgame_store(fresh_root, core_library, index_library):
    # Called by the future host integration after all acceptance is complete.
    # Reuse its existing Provider: no model duplication/download or query prefixing.
    from app.common.exceptions import AppError
    from app.services.ai import provider
    from app.services.ai.embedding_provider import DIMENSION, IDENTITY, QUERY_INSTRUCTION
    if __package__ and __package__.count(".") >= 2:
        from ...integrations.wordgame_rag import create_store
    else:
        from wordgame_rag import create_store
    return create_store(fresh_root, embed_with_provider=provider.embed_with_provider,
        identity=IDENTITY, dimension=DIMENSION, pooling="mean",
        query_transform="sha256:" + hashlib.sha256(QUERY_INSTRUCTION.encode()).hexdigest(),
        core_library=core_library, index_library=index_library,
        status=lambda: provider.embedding_backend().status(),
        warmup=lambda: provider.embedding_backend().ensure_ready(), error_factory=AppError)
