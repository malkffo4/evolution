# app/services/pdf_ingester.py
import fitz
import json
import tempfile
import os
import sys
from pathlib import Path

from core.base_service import BaseService
from knowledge.prompts import EXTRACTION_PROMPT

class PdfIngester(BaseService):
    def run(self, file_path: str, max_pages: int = 50):
        path = Path(file_path)
        if not path.exists():
            self.logger.error(f"File not found: {file_path}")
            return

        doc = fitz.open(path)
        for page_num in range(min(len(doc), max_pages)):
            self.logger.info(f"Processing page {page_num+1}...")
            page = doc[page_num]

            # 1. Извлечение текста
            text = page.get_text()
            if len(text.strip()) > 50:
                self._process_text_chunk(text)

            # 2. Извлечение и анализ картинок через Vision LLM
            images = self._extract_images(page)
            for i, img_bytes in enumerate(images):
                self._process_image(img_bytes, page_num, i)

    def _process_text_chunk(self, text: str):
        if not self.llm:
            return
        prompt = EXTRACTION_PROMPT.format(chunk=text[:2500])
        raw = self.llm.query(prompt, json_mode=True)
        try:
            graph = json.loads(raw)
            if graph.get("atoms"):
                self.send_to_core(graph)
        except json.JSONDecodeError:
            pass

    def _process_image(self, img_bytes: bytes, page_num: int, img_idx: int):
        if not self.llm:
            return

        with tempfile.NamedTemporaryFile(delete=False, suffix=".png") as tmp:
            tmp.write(img_bytes)
            tmp_path = tmp.name

        try:
            prompt = (
                "Ты когнитивный экстрактор. Изучи это изображение из книги/документа. "
                "Извлеки из схемы, графика или картинки все важные факты, концепции и связи между ними. "
                "Оформи их строго в соответствии с форматом NeuroAtom (с векторами truth, attention и т.д.).\n\n"
                + EXTRACTION_PROMPT.format(chunk="[ВИЗУАЛЬНЫЕ ДАННЫЕ С КАРТИНКИ]")
            )
            raw = self.llm.query(prompt, json_mode=True, image_path=tmp_path)
            graph = json.loads(raw)
            if graph.get("atoms"):
                self.send_to_core(graph)
                self.logger.info(f"Extracted {len(graph['atoms'])} atoms from image {img_idx+1} on page {page_num+1}")
        except Exception as e:
            self.logger.error(f"Failed to process image {img_idx+1} on page {page_num+1}: {e}")
        finally:
            os.unlink(tmp_path)

    @staticmethod
    def _extract_images(page) -> list:
        """
        Вместо извлечения внутренних объектов (которые часто нарезаны на полосы),
        рендерим всю страницу целиком как единое изображение, если на ней есть визуал.
        """
        # Если картинок на странице нет вообще, пропускаем (чтобы не гонять текст через Vision)
        if not page.get_images(full=True):
            return []

        # Рендерим страницу целиком.
        # Масштаб 2.0 (matrix) дает высокое разрешение для нейросети
        zoom_matrix = fitz.Matrix(2.0, 2.0)
        pix = page.get_pixmap(matrix=zoom_matrix, alpha=False)

        # Возвращаем байты одного цельного изображения страницы
        return [pix.tobytes("png")]
