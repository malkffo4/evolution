# models/local_embedding.py
from sentence_transformers import SentenceTransformer
model = SentenceTransformer('all-MiniLM-L6-v2') # "multilingual-e5-small"

def get_embedding(text):
    return model.encode(text).tolist()


    vector = model.encode(text)

    return {
        "vector": vector.tolist()
    }
