# app/services/pdf_ingester.py
import fitz
import json
from pathlib import Path
from core.base_service import BaseService

class PdfIngester(BaseService):
    def run(self, file_path: str, max_pages: int = 50):
        path = Path(file_path)
        if not path.exists():
            self.logger.error(f"File not found: {file_path}")
            return
        doc = fitz.open(path)
        for page_num in range(min(len(doc), max_pages)):
            page = doc[page_num]
            # 1. Извлечение текста
            text = page.get_text()
            if len(text.strip()) > 50:
                self._process_text_chunk(text)
            # 2. Извлечение картинок (при необходимости)
            images = self._extract_images(page)
            for img in images:
                # Здесь можно отправить img в LLM для описания
                pass

    def _process_text_chunk(self, text: str):
        if not self.llm:
            return
        prompt = f"Извлеки факты (nodes/edges) из:\n{text[:1500]}"
        raw = self.llm.query(prompt)
        try:
            graph = json.loads(raw)
            self.send_to_core(graph)
        except json.JSONDecodeError:
            pass

    @staticmethod
    def _extract_images(page) -> list:
        """Извлекает все изображения со страницы и возвращает их байты."""
        images = []
        for img_info in page.get_images(full=True):
            xref = img_info[0]
            base_image = page.parent.extract_image(xref)
            images.append(base_image["image"])  # байты PNG/JPEG
        return images
