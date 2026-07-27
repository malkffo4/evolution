# models/embedding.py
from sentence_transformers import SentenceTransformer
model = SentenceTransformer('intfloat/paraphrase-multilingual-mpnet-base-v2')

def get_embedding(text: str, node_id: str = None) -> dict:
    vector = model.encode(text)

    payload = {
        "text": text,
        "vector": vector.tolist(),
        "node_id": node_id  # если известен хеш узла
    }

    return payload
