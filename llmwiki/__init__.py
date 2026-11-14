"""FengquDB-backed, incremental and provider-independent LLM Wiki."""

from .wiki import Wiki, WikiError

__all__ = ["Wiki", "WikiError"]
__version__ = "0.1.0"
