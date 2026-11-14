"""Unicode character slices for immutable source citations; no search/tokenizer."""


def chunks(text, size=1800, overlap=160):
    start = 0
    while start < len(text):
        end = min(start + size, len(text))
        if end < len(text):
            boundary = text.rfind("\n", start + size // 2, end)
            if boundary != -1:
                end = boundary + 1
        yield start, end, text[start:end]
        if end == len(text):
            break
        start = end - overlap
