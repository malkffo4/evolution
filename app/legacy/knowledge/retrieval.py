from duckduckgo_search import DDGS
import evolution.app.knowledge.sources.wikipedia as wikipedia

from llm.ollama import Local_LLM

class ResearchService:
    def __init__(self):
        self.llm = Local_LLM()
        wikipedia.set_lang("ru")

    def wikipedia(self, query: str):
        try:
            page = wikipedia.page(query, auto_suggest=True)

            return {
                "source": "wikipedia",
                "title": page.title,
                "summary": wikipedia.summary(query, sentences=5),
                "url": page.url
            }
        except Exception:
            return None

    def web(self, query: str):
        try:
            with DDGS() as ddgs:
                results = list(ddgs.text(query, max_results=5))
            return {
                "source": "duckduckgo",
                "results": results
            }
        except Exception:
            return None

    def answer(self, query: str):
        prompt = f"""Используй свои знания.
        Если информации недостаточно — так и скажи.
        Вопрос: {query}
        """

        return self.llm.process_chat(prompt)

    def handle(self, query: str):
        context = []

        wiki = self.wikipedia(query)
        if wiki:
            context.append(wiki)

        web = self.web(query)
        if web:
            context.append(web)

        answer = self.llm.answer(query, context)

        return {
            "source": "llm",
            "context": context,
            "answer": answer
        }

service = ResearchService()

@request("research")
def handle(payload):
    query = payload["query"]

    return service.handle(query)
