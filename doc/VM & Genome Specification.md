# **Спецификация Байт-код VM и ДНК Мышления ( v1)**

Мы отказываемся от жестких алгоритмов. Любой мыслительный акт (дедукция, аналогия, индукция) — это программа (пайплайн), выполняемая нашей виртуальной машиной над регистрами Рабочей Памяти.

## **1\. Типы регистров (Typed Blackboard Slots)**

Каждое выполнение пайплайна инициализирует контекст с жестко типизированными регистрами:

| Номер регистра | Тип данных | Описание |
| :---- | :---- | :---- |
| REG\_NODE | node\_id\_t | Одиночный концепт (активный фокус) |
| REG\_NODE\_SET | node\_id\_t\* \+ count | Набор узлов (результаты поиска, кандидаты) |
| REG\_EDGE\_SET | Edge\* \+ count | Набор связей (выборка из БД) |
| REG\_SCORE | float | Оценка уверенности / релевантности |
| REG\_EMBEDDING | float\[768\] | Семантический вектор |

## **2\. Базовые опкоды (Примитивы на Си)**

Наша VM выполняет операции над этими регистрами.

### **Навигация и поиск по графу:**

* OP\_GET\_OUTGOING(in: REG\_NODE, out: REG\_EDGE\_SET) — вытащить все исходящие связи.  
* OP\_GET\_INCOMING(in: REG\_NODE, out: REG\_EDGE\_SET) — вытащить все входящие связи.  
* OP\_EXTRACT\_TARGETS(in: REG\_EDGE\_SET, out: REG\_NODE\_SET) — собрать все узлы-цели из набора связей.

### **Фильтрация и математика:**

* OP\_FILTER\_BY\_RELATION(in: REG\_EDGE\_SET, relation: string\_id, out: REG\_EDGE\_SET) — оставить связи только определенного типа.  
* OP\_COMPUTE\_SIMILARITY(in: REG\_NODE, in: REG\_NODE, out: REG\_SCORE) — расстояние Хэмминга по SimHash.  
* OP\_TOP\_K(in: REG\_NODE\_SET, k: int, out: REG\_NODE\_SET) — оставить K лучших по уровню активации.

### **Логика и гипотезы:**

* OP\_MATCH\_EDGES(in: REG\_EDGE\_SET, in: REG\_EDGE\_SET, out: REG\_SCORE) — пересечение паттернов.  
* OP\_CREATE\_HYPOTHESIS(source: REG\_NODE, target: REG\_NODE, relation: string\_id, confidence: REG\_SCORE) — запись новой гипотетической связи.

## **3\. ДНК Мышления (Геном)**

Любая стратегия — это массив опкодов. Например:

* **Аналогия (ДНК: \[OP\_GET\_OUTGOING, OP\_FILTER\_BY\_RELATION, OP\_COMPUTE\_SIMILARITY, OP\_CREATE\_HYPOTHESIS\])**  
* **Дедукция (ДНК: \[OP\_GET\_OUTGOING, OP\_EXTRACT\_TARGETS, OP\_GET\_OUTGOING, OP\_CREATE\_HYPOTHESIS\])**
