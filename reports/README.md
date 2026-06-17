# Отчёты: динамический анализ (ASAN) и профилирование

Сравнение «до / после» работы утилиты `refactor_tool` на двух примерах из ТЗ:
`tests/tests_data/leak_example.cpp` и `tests/tests_data/perf_example.cpp`.

Версии «после» получены запуском утилиты:

```bash
./build/refactor_tool reports/leak_example_after.cpp --
./build/refactor_tool reports/perf_example_after.cpp --
```

## 1. AddressSanitizer — `leak_example.cpp`

В заготовке репозитория деструктор `Base` уже помечен `virtual`, поэтому версия
«до» (`leak_example_before.cpp`) — это заготовка с убранным `virtual`
(как и описано в ТЗ: невиртуальный деструктор базового класса).

| Файл | Содержимое |
|---|---|
| `leak_example_before.cpp` | версия до рефакторинга (`~Base()` не виртуальный) |
| `leak_example_after.cpp` | версия после прогона `refactor_tool` (добавлен `virtual ~Base()`) |
| `refactor_tool_leak_log.txt` | лог утилиты (remark о правке) |
| `asan_before.txt` | вывод программы, собранной `g++ -fsanitize=address -g`, ДО |
| `asan_after.txt` | то же ПОСЛЕ |

Результат:

- **До**: `AddressSanitizer: new-delete-type-mismatch` — при `delete obj` через
  `Base*` вызывается только `~Base()`, деструктор `Derived` не вызывается,
  массив `data` не освобождается; программа аварийно завершается (exit 1).
- **После**: ошибок нет, exit 0 — полиморфное удаление корректно.

Дополнительно весь проект собран с `-fsanitize=address` (папка `build-asan`),
юнит-тесты под ASAN: 7/7 PASSED
(запуск с `ASAN_OPTIONS=allow_user_poisoning=0` из-за ложного use-after-poison
при смешивании инструментированных заголовков LLVM с системной `libLLVM.so`).

## 2. Профилирование — `perf_example.cpp`

| Файл | Содержимое |
|---|---|
| `perf_example_before.cpp` | версия до рефакторинга (`for (const auto obj : vec)` — копирование) |
| `perf_example_after.cpp` | версия после прогона `refactor_tool` (`for (const auto& obj : vec)`) |
| `refactor_tool_perf_log.txt` | лог утилиты (remark о правке) |
| `time_measurements.txt` | замеры `/usr/bin/time` (3 прогона каждой версии, `g++ -O2`) |
| `callgrind_comparison.txt` | сравнение профилей valgrind/callgrind до и после |

Результаты:

- **callgrind** (контейнер уменьшен до 5000 объектов, чтобы прогон занимал секунды):
  - до: **372 116 703** инструкций; в топе — копирование строк
    (`basic_string.h`/`basic_string.tcc`/`char_traits.h` в `main`, 115M Ir)
    и уничтожение временных копий `HeavyObject` (40M Ir в `~vector`/`~basic_string`);
  - после: **237 047 568** инструкций (**−36%**); копирования и деструкторы
    временных объектов из профиля исчезли полностью; остался только
    одинаковый для обеих версий `memset` первоначального конструирования
    вектора (160M Ir).
- **/usr/bin/time** (полный размер, 100000 объектов): user time стабильно
  меньше после рефакторинга (~0.35–0.40 s → ~0.21–0.24 s); elapsed time
  зашумлён, так как доминирует конструирование вектора на ~3.2 ГБ,
  одинаковое в обеих версиях.
