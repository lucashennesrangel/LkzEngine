# third_party (opcional)

Normalmente vazio: as dependências são baixadas pelo FetchContent para dentro da pasta de build.

Para trabalhar offline ou testar um fork, coloque um clone aqui com o nome da dependência (`entt`, `glm`, `spdlog` ou `tracy`). O `cmake/Dependencies.cmake` usa o clone local no lugar do download. Exemplo:

```
git clone --depth 1 --branch v4.0.0 https://github.com/skypjack/entt.git third_party/entt
```

Apague a pasta de build (ou o cache do CMake) depois de adicionar ou remover um clone.
