# FIM offline eval (HumanEval-Infilling, greedy exact-match)

- date: 2026-10-03 17:17:25
- engine: local Ollama `http://localhost:11434` (`/api/generate`, `raw=true`), 순차 실행
- prompt: `<|fim_prefix|>{prefix}<|fim_suffix|>{suffix}<|fim_middle|>` (Qwen PSM)
- sampling: greedy (temperature=0, n=1), num_predict=256, stop=['<|endoftext|>', '<|fim_pad|>']
- metric: exact-match rate (postprocessed completion == canonical_solution, strip 비교). greedy n=1 이므로 exact-match rate == pass@1.
- postprocess: single_line=첫 `\n` 절단, multi_line/random_span=suffix와 첫 일치하는 줄에서 절단
- data: HumanEval-Infilling, subset별 선두 50 샘플

## Results

| model | subset | n | exact match | EM rate | avg latency (s) | total (s) | errors |
|---|---|---|---|---|---|---|---|
| `lvc-coder:latest` | multi_line | 50 | 2 | 4.0% | 10.15 | 507.3 | 0 |
| `lvc-coder:latest` | random_span | 50 | 6 | 12.0% | 4.02 | 201.2 | 0 |
| `lvc-coder:latest` | single_line | 50 | 30 | 60.0% | 10.48 | 523.9 | 2 |
| `qwen2.5-coder:3b` | multi_line | 50 | 14 | 28.0% | 3.14 | 156.8 | 0 |
| `qwen2.5-coder:3b` | random_span | 50 | 23 | 46.0% | 3.14 | 157.0 | 0 |
| `qwen2.5-coder:3b` | single_line | 50 | 41 | 82.0% | 1.63 | 81.7 | 0 |

## Samples (raw vs postprocessed vs canonical)

### `lvc-coder:latest` — multi_line

- `MultiLineInfilling/HumanEval/0/L0_L0` (EM)
  - raw:

~~~
    for idx, elem in enumerate(numbers):
        for idx2, elem2 in enumerate(numbers):
            if idx != idx2:
                distance = abs(elem - elem2)
                if distance < threshold:
                    return True
        else:
            continue
        for idx2, elem2 in enumerate(numbers):
            if idx != idx2:
                distance = abs(elem - elem2)
                if distance < threshold:
                    return True
        else:
            continue
        for idx2, elem2 in enumerate(numbers):
            if idx != idx2:
                distance = abs(elem - elem2)
                if distance < threshold:
                    return True
        else:
            continue
        for idx2, elem2 in enumerate(numbers):
            if idx != idx2:
                distance = abs(elem - elem2)
                if distance < threshold:
                    return True
        else:
            continue
        for idx2, elem2 in enumerate(numbers):
            if idx != idx2:
                distance = abs(elem - elem2)
                if distance < threshold:
                    return True
        else:
            continue
        for idx2, elem2 in enumerate(numbers):
            if idx != idx2:
                distance = abs(elem - elem2)
                if distance < threshold~~~
  - postprocessed:

~~~
    for idx, elem in enumerate(numbers):~~~
  - canonical:

~~~
    for idx, elem in enumerate(numbers):
~~~

- `MultiLineInfilling/HumanEval/0/L0_L1` (miss)
  - raw:

~~~
    for idx, elem in enumerate(numbers):
        for idx2, elem2 in enumerate(numbers[idx + 1:]):
            for idx3, elem3 in enumerate(numbers[idx2 + 1:]):
                for idx4, elem4 in enumerate(numbers[idx3 + 1:]):
                    for idx5, elem5 in enumerate(numbers[idx4 + 1:]):
                        for idx6, elem6 in enumerate(numbers[idx5 + 1:]):
                            for idx7, elem7 in enumerate(numbers[idx6 + 1:]):
                                for idx8, elem8 in enumerate(numbers[idx7 + 1:]):
                                    for idx9, elem9 in enumerate(numbers[idx8 + 1:]):
                                        for idx10, elem10 in enumerate(numbers[idx9 + 1:]):
                                            for idx11, elem11 in enumerate(numbers[idx10 + 1:])
                                                for idx12, elem12 in enumerate(numbers[idx11 + 1:])
                                                for idx13, elem13 in enumerate(numbers[idx12 + 1:])
                                                for idx14, elem14 in enumerate(numbers[idx13 + 1:])
                                                for idx15, elem15 in enumerate(numbers[idx14 + ~~~
  - postprocessed:

~~~
    for idx, elem in enumerate(numbers):
        for idx2, elem2 in enumerate(numbers[idx + 1:]):
            for idx3, elem3 in enumerate(numbers[idx2 + 1:]):
                for idx4, elem4 in enumerate(numbers[idx3 + 1:]):
                    for idx5, elem5 in enumerate(numbers[idx4 + 1:]):
                        for idx6, elem6 in enumerate(numbers[idx5 + 1:]):
                            for idx7, elem7 in enumerate(numbers[idx6 + 1:]):
                                for idx8, elem8 in enumerate(numbers[idx7 + 1:]):
                                    for idx9, elem9 in enumerate(numbers[idx8 + 1:]):
                                        for idx10, elem10 in enumerate(numbers[idx9 + 1:]):
                                            for idx11, elem11 in enumerate(numbers[idx10 + 1:])
                                                for idx12, elem12 in enumerate(numbers[idx11 + 1:])
                                                for idx13, elem13 in enumerate(numbers[idx12 + 1:])
                                                for idx14, elem14 in enumerate(numbers[idx13 + 1:])
                                                for idx15, elem15 in enumerate(numbers[idx14 + ~~~
  - canonical:

~~~
    for idx, elem in enumerate(numbers):
        for idx2, elem2 in enumerate(numbers):
~~~

- `MultiLineInfilling/HumanEval/0/L0_L2` (miss)
  - raw:

~~~
    for (i, elem) in enumerate(numbers):
        for (j, elem2) in enumerate(numbers[i + 1:]):
            if elem2 == elem:
                continue
            else:
                for (k, elem3) in enumerate(numbers[i + 1:]):
                    if elem3 == elem2:
                        continue
                    else:
                        if k == len(numbers[i + 1:]) - 1:
                            break
                        else:
                            if elem3 == elem2:
                                continue
                            else:
                                if k == len(numbers[i + 1:]) - 2:
                                    break
                                else:
                                    k += 1
                                    continue
                            else:
                                k += 1
                                continue
                            else:
                                break
                            else:
                                break
                            else:
                                break
                            else:
                                break
                            else:
                                break
                            else:
                                break
                            else:
                                break
                            else:
                                break
                            else:
                                break
                            else:
                                break
                            else:
                                break
                            else:
                                break
                            else:
                                break
                            else:
                                break
                            else:
                                break
                            else:
                                break
                            else:
                                break
                            else:
                                break~~~
  - postprocessed:

~~~
    for (i, elem) in enumerate(numbers):
        for (j, elem2) in enumerate(numbers[i + 1:]):
            if elem2 == elem:
                continue
            else:
                for (k, elem3) in enumerate(numbers[i + 1:]):
                    if elem3 == elem2:
                        continue
                    else:
                        if k == len(numbers[i + 1:]) - 1:
                            break
                        else:
                            if elem3 == elem2:
                                continue
                            else:
                                if k == len(numbers[i + 1:]) - 2:
                                    break
                                else:
                                    k += 1
                                    continue
                            else:
                                k += 1
                                continue
                            else:
                                break
                            else:
                                break
                            else:
                                break
                            else:
                                break
                            else:
                                break
                            else:
                                break
                            else:
                                break
                            else:
                                break
                            else:
                                break
                            else:
                                break
                            else:
                                break
                            else:
                                break
                            else:
                                break
                            else:
                                break
                            else:
                                break
                            else:
                                break
                            else:
                                break
                            else:
                                break~~~
  - canonical:

~~~
    for idx, elem in enumerate(numbers):
        for idx2, elem2 in enumerate(numbers):
            if idx != idx2:
~~~

### `lvc-coder:latest` — random_span

- `RandomSpanInfilling/HumanEval/0/1` (miss)
  - raw:

~~~
 idx2 > idx:
                distance = abs(elem - elem2)
                if distance < threshold:
                    return True
                elif idx2 == len(numbers)-1:
                    if idx2 == idx:
                        continue
                    else:
                        break
                else:
                    idx2 += 1
                    continue
                break
            else:
                break
        else:
            continue
        break
        else:
            return False
        break
        else:
            return False
        break
        else:
            return False
        break
        else:
            return False
        break
        else:
            return False
        break
        else:
            return False
        break
        else:
            return False
        break
        else:
            return False
        break
        else:
            return False
        break
        else:
            return False
        break
        else:
            return False
        break
        else:
            return False
        break
        else:
            return False
        break
        else:
            return False
        break
        else:
            return False
        break
        else:
            return False
        break
        else:
            return False
        break
        else:
            return~~~
  - postprocessed:

~~~
 idx2 > idx:
                distance = abs(elem - elem2)
                if distance < threshold:~~~
  - canonical:

~~~
 idx != idx2:
                distance = abs(elem - elem2)
                if~~~

- `RandomSpanInfilling/HumanEval/0/2` (miss)
  - raw:

~~~
 in enumerate(numbers):
        for idx2, elem2 in enumerate(numbers[idx + 1:]):
            for e2 in numbers[idx2 + 1:]:
                if e2 == e:
                    continue
                else:
                    e2 = e2
                    break
                else:
                    e2 = e2 + threshold
                    break
                else:
                    e2 = e2 + threshold
                    break
                else:
                    e2 = e2 + threshold
                    break
                else:
                    e2 = e2 + threshold
                    break
                else:
                    e2 = e2 + threshold
                    break
                else:
                    e2 = e2 + threshold
                    break
                else:
                    e2 = e2 + threshold
                    break
                else:
                    e2 = e2 + threshold
                    break
                else:
                    e2 = e2 + threshold
                    break
                else:
                    e2 = e2 + threshold
                    break
                else:
                    e2 = e2 + threshold
                    break
                else:
                    e2 = e2 + threshold
                    break
                else:
                    e2 = e2 + threshold
                    break
                else:
                    e2~~~
  - postprocessed:

~~~
 in enumerate(numbers):
        for idx2, elem2 in enumerate(numbers[idx + 1:]):
            for e2 in numbers[idx2 + 1:]:
                if e2 == e:
                    continue
                else:
                    e2 = e2
                    break
                else:
                    e2 = e2 + threshold
                    break
                else:
                    e2 = e2 + threshold
                    break
                else:
                    e2 = e2 + threshold
                    break
                else:
                    e2 = e2 + threshold
                    break
                else:
                    e2 = e2 + threshold
                    break
                else:
                    e2 = e2 + threshold
                    break
                else:
                    e2 = e2 + threshold
                    break
                else:
                    e2 = e2 + threshold
                    break
                else:
                    e2 = e2 + threshold
                    break
                else:
                    e2 = e2 + threshold
                    break
                else:
                    e2 = e2 + threshold
                    break
                else:
                    e2 = e2 + threshold
                    break
                else:
                    e2 = e2 + threshold
                    break
                else:
                    e2~~~
  - canonical:

~~~
lem in enumerate(numbers):
        for idx2, elem2 in enumerate(numbers):
    ~~~

- `RandomSpanInfilling/HumanEval/0/3` (miss)
  - raw:

~~~
rese(numbers, idx + 1):
            if idx2~~~
  - postprocessed:

~~~
rese(numbers, idx + 1):
            if idx2~~~
  - canonical:

~~~
merate(numbers):
            if idx~~~

### `lvc-coder:latest` — single_line

- `SingleLineInfilling/HumanEval/0/L0` (EM)
  - raw:

~~~
    for idx, elem in enumerate(numbers):
        for idx2, elem2 in enumerate(numbers):
            if idx != idx2:
                distance = abs(elem - elem2)
                if distance < threshold:
                    return True
        else:
            continue
        for idx2, elem2 in enumerate(numbers):
            if idx != idx2:
                distance = abs(elem - elem2)
                if distance < threshold:
                    return True
        else:
            continue
        for idx2, elem2 in enumerate(numbers):
            if idx != idx2:
                distance = abs(elem - elem2)
                if distance < threshold:
                    return True
        else:
            continue
        for idx2, elem2 in enumerate(numbers):
            if idx != idx2:
                distance = abs(elem - elem2)
                if distance < threshold:
                    return True
        else:
            continue
        for idx2, elem2 in enumerate(numbers):
            if idx != idx2:
                distance = abs(elem - elem2)
                if distance < threshold:
                    return True
        else:
            continue
        for idx2, elem2 in enumerate(numbers):
            if idx != idx2:
                distance = abs(elem - elem2)
                if distance < threshold~~~
  - postprocessed:

~~~
    for idx, elem in enumerate(numbers):~~~
  - canonical:

~~~
    for idx, elem in enumerate(numbers):
~~~

- `SingleLineInfilling/HumanEval/0/L1` (miss)
  - raw:

~~~
        for idx2 in range(idx, len(numbers)):
            if idx2 == idx:
                continue
            else:
                elem2 = numbers[idx2]
                if idx2 == len(numbers)-1:
                    break
                else:
                    idx2 += 1
                    continue
                else:
                    idx2 += 1
                    continue
                else:
                    idx2 += 1
                    continue
                else:
                    idx2 += 1
                    continue
                else:
                    idx2 += 1
                    continue
                else:
                    idx2 += 1
                    continue
                else:
                    idx2 += 1
                    continue
                else:
                    idx2 += 1
                    continue
                else:
                    idx2 += 1
                    continue
                else:
                    idx2 += 1
                    continue
                else:
                    idx2 += 1
                    continue
                else:
                    idx2 += 1
                    continue
                else:
                    idx2 += 1
                    continue
                else:
                    idx2 += 1
                    continue
                else:
                    idx2 += 1
                    continue
                else:
                    idx2 += 1
                    continue
                else:
~~~
  - postprocessed:

~~~
        for idx2 in range(idx, len(numbers)):~~~
  - canonical:

~~~
        for idx2, elem2 in enumerate(numbers):
~~~

- `SingleLineInfilling/HumanEval/0/L2` (miss)
  - raw:

~~~
            if idx2 > idx:
                if elem == elem2:
                    continue
                else:
                    for idx3, elem3 in enumerate(numbers):
                        if idx3 > idx2:
                            if elem3 == elem2:
                                continue
                            else:
                                if elem3 == elem4:
                                    continue
                                else:
                                    elem4 = elem3
                                    continue
                            else:
                                elem4 = elem3
                                continue
                        else:
                            elem4 = elem3
                            continue
                    else:
                        elem4 = elem3
                        continue
                else:
                    elem4 = elem3
                    continue
            else:
                elem4 = elem3
                continue
        else:
            elem4 = elem3
            continue
    else:
        return False
    else:
        for idx, elem in enumerate(numbers):
            for idx2, elem2 in enumerate(numbers):
                if idx2 > idx:
                    if elem2 == elem:
                        continue
                    else:
                        if elem2 == elem3:
                            continue
                        else:
                            elem3 = elem2
                            continue
                else:
                    elem3 = elem2
                    continue
            else:
                return False
            else:
                for idx3~~~
  - postprocessed:

~~~
            if idx2 > idx:~~~
  - canonical:

~~~
            if idx != idx2:
~~~

### `qwen2.5-coder:3b` — multi_line

- `MultiLineInfilling/HumanEval/0/L0_L0` (EM)
  - raw:

~~~
    for idx, elem in enumerate(numbers):
~~~
  - postprocessed:

~~~
    for idx, elem in enumerate(numbers):
~~~
  - canonical:

~~~
    for idx, elem in enumerate(numbers):
~~~

- `MultiLineInfilling/HumanEval/0/L0_L1` (EM)
  - raw:

~~~
    for idx, elem in enumerate(numbers):
        for idx2, elem2 in enumerate(numbers):
~~~
  - postprocessed:

~~~
    for idx, elem in enumerate(numbers):
        for idx2, elem2 in enumerate(numbers):
~~~
  - canonical:

~~~
    for idx, elem in enumerate(numbers):
        for idx2, elem2 in enumerate(numbers):
~~~

- `MultiLineInfilling/HumanEval/0/L0_L2` (miss)
  - raw:

~~~
    for elem in numbers:
        for elem2 in numbers:
            if elem != elem2:
~~~
  - postprocessed:

~~~
    for elem in numbers:
        for elem2 in numbers:
            if elem != elem2:
~~~
  - canonical:

~~~
    for idx, elem in enumerate(numbers):
        for idx2, elem2 in enumerate(numbers):
            if idx != idx2:
~~~

### `qwen2.5-coder:3b` — random_span

- `RandomSpanInfilling/HumanEval/0/1` (miss)
  - raw:

~~~
 idx != idx2 and abs(elem - elem2) < threshold:
                return True

    return False


def has_close_elements_v2(numbers: List[float], threshold: float) -> bool:
    """ Check if in given list of numbers, are any two numbers closer to each other than
    given threshold.
    >>> has_close_elements_v2([1.0, 2.0, 3.0], 0.5)
    False
    >>> has_close_elements_v2([1.0, 2.8, 3.0, 4.0, 5.0, 2.0], 0.3)
    True
    """
    for idx, elem in enumerate(numbers):
        distance = abs(elem - numbers[idx + 1])
        if distance < threshold:
            return True

    return False


def has_close_elements_v3(numbers: List[float], threshold: float) -> bool:
    """ Check if in given list of numbers, are any two numbers closer to each other than
    given threshold.
    >>> has_close_elements_v3([1.0, 2.0, 3.0], 0.5)
    False
    >>> has_close_elements_v3([~~~
  - postprocessed:

~~~
 idx != idx2 and abs(elem - elem2) < threshold:~~~
  - canonical:

~~~
 idx != idx2:
                distance = abs(elem - elem2)
                if~~~

- `RandomSpanInfilling/HumanEval/0/2` (EM)
  - raw:

~~~
lem in enumerate(numbers):
        for idx2, elem2 in enumerate(numbers):
    ~~~
  - postprocessed:

~~~
lem in enumerate(numbers):
        for idx2, elem2 in enumerate(numbers):~~~
  - canonical:

~~~
lem in enumerate(numbers):
        for idx2, elem2 in enumerate(numbers):
    ~~~

- `RandomSpanInfilling/HumanEval/0/3` (EM)
  - raw:

~~~
merate(numbers):
            if idx~~~
  - postprocessed:

~~~
merate(numbers):
            if idx~~~
  - canonical:

~~~
merate(numbers):
            if idx~~~

### `qwen2.5-coder:3b` — single_line

- `SingleLineInfilling/HumanEval/0/L0` (EM)
  - raw:

~~~
    for idx, elem in enumerate(numbers):
~~~
  - postprocessed:

~~~
    for idx, elem in enumerate(numbers):~~~
  - canonical:

~~~
    for idx, elem in enumerate(numbers):
~~~

- `SingleLineInfilling/HumanEval/0/L1` (EM)
  - raw:

~~~
        for idx2, elem2 in enumerate(numbers):
~~~
  - postprocessed:

~~~
        for idx2, elem2 in enumerate(numbers):~~~
  - canonical:

~~~
        for idx2, elem2 in enumerate(numbers):
~~~

- `SingleLineInfilling/HumanEval/0/L2` (EM)
  - raw:

~~~
            if idx != idx2:
~~~
  - postprocessed:

~~~
            if idx != idx2:~~~
  - canonical:

~~~
            if idx != idx2:
~~~

