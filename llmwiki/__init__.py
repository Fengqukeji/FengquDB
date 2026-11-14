"""FengquDB-backed, incremental and provider-independent LLM Wiki."""

from .wiki import Wiki, WikiError
from .index_provider import IndexDefinition, IndexProvider, PythonIndexProvider

__all__ = ["Wiki", "WikiError", "IndexDefinition", "IndexProvider", "PythonIndexProvider"]
__version__ = "0.1.0"
