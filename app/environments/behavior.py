import time
import random
import numpy as np

class HumanBehavior:
    @staticmethod
    def _bezier_curve(start, end, points=30):
        t = np.linspace(0, 1, points)
        cp = (start[0] + random.randint(-100, 100), start[1] + random.randint(-100, 100))
        x = (1 - t)**2 * start[0] + 2 * (1 - t) * t * cp[0] + t**2 * end[0]
        y = (1 - t)**2 * start[1] + 2 * (1 - t) * t * cp[1] + t**2 * end[1]
        return list(zip(x, y))

    @staticmethod
    def simulate_mouse_move(page, start_pos, end_pos):
        curve = HumanBehavior._bezier_curve(start_pos, end_pos, points=random.randint(15, 35))
        for x, y in curve:
            page.actions.move_to((int(x), int(y)))
            time.sleep(random.uniform(0.005, 0.015))

    @staticmethod
    def human_type(element, text: str):
        for char in text:
            delay = random.uniform(0.05, 0.15)
            if random.random() < 0.05:
                delay += random.uniform(0.2, 0.5)
            element.input(char)
            time.sleep(delay)
