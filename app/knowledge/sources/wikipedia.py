import wikipediaapi # pip install wikipedia-api

def _get_wiki_summary(query):
    wiki = wikipediaapi.Wikipedia('Core/1.0', 'ru')
    page = wiki.page(query)
    if page.exists():
        return page.summary[:1500]

def explain_unknown_word(unknown_word:str):
    summary = _get_wiki_summary(unknown_word)
    if summary:
        graph_data = _analyze_with_llm(unknown_word, summary)
        if graph_data:
            return graph_data
