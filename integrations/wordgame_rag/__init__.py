"""Explicit dependency-injection facade; never imports or edits the game project."""
if __package__ and __package__.count(".") >= 2:
    # FengquDB.integrations.wordgame_rag (also works below a host namespace).
    from ...bindings.python.fengqudb_rag import MemoryStore, EmbeddingContract, CallbackEmbedding, RagError
else:
    from fengqudb_rag import MemoryStore, EmbeddingContract, CallbackEmbedding, RagError


class WordGameMemoryStore:
    def __init__(self, engine, error_factory=None):
        self.engine, self.error_factory = engine, error_factory

    def __getattr__(self, name):
        target = getattr(self.engine, name)
        if not callable(target) or self.error_factory is None:
            return target
        def translated(*args, **kwargs):
            try:
                return target(*args, **kwargs)
            except (RagError, ValueError, TypeError) as exc:
                raise self.error_factory(str(exc)) from exc
        return translated


def create_store(root, *, embed_with_provider, identity, dimension, core_library=None,
                 index_library=None, index=None, status=None, warmup=None, error_factory=None,
                 pooling="mean", document_transform="document-v1", query_transform="retrieval-v1", **options):
    """Reuse the host's lazy KaLM callback; host continues owning model close()."""
    embedding = CallbackEmbedding(embed_with_provider,
        EmbeddingContract(identity, dimension, pooling=pooling,
                          document_transform=document_transform, query_transform=query_transform),
        status=status, warmup=warmup)
    engine = MemoryStore(root, embedding=embedding, core_library=core_library,
        index_library=index_library, index=index, clock_id="wordgame-hour-v1", **options)
    return WordGameMemoryStore(engine, error_factory)
