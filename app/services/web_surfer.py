# app/services/web_surfer.py
# pip install playwright && playwright install
from playwright.sync_api import sync_playwright
from core.base_service import BaseService

class WebSurfer(BaseService):
    def search_and_extract(self, query: str) -> str:
        with sync_playwright() as p:
            browser = p.chromium.launch()
            page = browser.new_page()
            page.goto(f"https://duckduckgo.com/?q={query}")
            page.wait_for_selector(".result__body")
            results = page.query_selector_all(".result__body")
            text = "\n".join([r.inner_text() for r in results[:5]])
            browser.close()
            return text
