# Rexon

Rexon 1.0 es un lenguaje pequeño que se transpila a C++26. El compilador se llama
`rexonc` y usa `clang++` para generar el ejecutable final.

## Compilar el compilador

Desde este directorio:

```sh
clang++ -std=c++26 -O2 main.cpp cli.cpp -o rexonc
```

También se puede generar el header estándar incluido por Rexon:

```sh
./rexonc --get-std-hpp > std.hpp
```

## Primer programa

Guarda el siguiente código como `hello.rxn`:

```rexon
fn main(char[][] args) {
    @print("Hola, {}!\n", "Rexon");
    @print("Ejecutable: {}\n", args[0]);
}
```

Compílalo y ejecútalo:

```sh
./rexonc hello.rxn -o hello
./hello
```

Si no se indica `-o`, el ejecutable recibe el nombre del archivo de entrada sin
la extensión `.rxn`.

Para conservar el C++ generado, usa `--emit-cpp`:

```sh
./rexonc --emit-cpp hello.rxn -o hello.cpp
```

## Sintaxis básica

La sintaxis es intencionalmente cercana a C++.

### Punto de entrada

El punto de entrada recomendado es `fn main(char[][] args)`. `args` contiene
los argumentos de la línea de comandos como un array dinámico de strings; el
primer elemento (`args[0]`) es el nombre del ejecutable. También se acepta
`fn main()` cuando no se necesitan argumentos. Si se omite el tipo de retorno,
`main` retorna `int` automáticamente.

```rexon
fn main(char[][] args) {
    if (args.size() > 1 && args.is_valid_idx(1)) {
        @print("Primer argumento: {}\n", args[1]);
    }
}
```

### Funciones

```rexon
fn add(int a, int b) int {
    return a + b;
}

fn log_message(const char[] message) {
    @print("{}\n", message);
}
```

El tipo de retorno va después de la lista de parámetros. Si se omite, la
función retorna `void`. `export fn`, `export struct` y `export enum` se aceptan
para marcar declaraciones públicas de un módulo.

### Variables, punteros y arrays

```rexon
int count = 42;
const char[6] name = "Rexon";
int[3] values = {10, 20, 30};

int* pointer = &count;
@assert(*pointer == 42, "pointer");
@print("{} {}\n", values[0], name);
```

Los arrays fijos usan `tipo[tamaño]`. Los punteros usan `tipo*` y el acceso a
miembros de una instancia usa `.`; dentro de los métodos, `this.member` se
convierte automáticamente a `this->member`.

### Structs, métodos y enums

```rexon
struct Counter {
    int value = 0;

    fn increment() {
        this.value += 1;
    }

    static fn create(int initial) Counter {
        return Counter{initial};
    }
}

enum Result {
    Ok,
    Error
}

Counter counter = Counter::create(10);
counter.increment();
```

Los métodos pueden ser normales o `static fn`. La inicialización de miembros y
la inicialización agregada de structs siguen las reglas de C++.

### Alias de tipos externos

`extern_type` crea un alias Rexon para un tipo C++ existente. El tipo de la
derecha se copia al C++ generado como una declaración `using`:

```rexon
extern_type str = std::string;

fn greeting(str name) {
    @print("Hola, {}!\n", name);
}
```

Por ejemplo, el código anterior genera aproximadamente:

```cpp
using str = std::string;
```

El header estándar ya incluye `<string>`. Para tipos definidos en otros headers,
inclúyelos con `@cpp` antes de declarar el alias:

```rexon
@cpp("#include <map>");
extern_type table = std::map<int, int>;
```

También se admite `export extern_type` cuando el alias forma parte de la API de
un módulo.

### Condicionales y bucles

Se admiten `if`, `else`, `while`, `for` y `switch` con sintaxis de C++:

```rexon
for (int i = 0; i < 3; i += 1) {
    @print("{}\n", i);
}

if (count > 0) {
    @print("hay elementos\n");
} else {
    @print("vacío\n");
}
```

También están disponibles `return`, `break` y `continue`.

### Opcionales

Un tipo terminado en `?` es opcional. Se usa `null` para representar ausencia y
`.?` para obtener el valor:

```rexon
fn divide(int a, int b) int? {
    if (b == 0) return null;
    return a / b;
}

int? result = divide(20, 4);
if (result) {
    @print("resultado: {}\n", result.?);
}
```

Internamente los opcionales se generan como `std::optional`.

## Memoria dinámica

Las operaciones `new`, `delete` y `resize` reciben un puntero `bool` para
informar si la operación tuvo éxito:

```rexon
bool ok = false;
int* value = new(&ok) int{10};
if (ok) {
    @print("{}\n", *value);
}
delete value;

int[] values = new(&ok) int[]{10, 20, 30};
resize(&ok) int[+2]{values, 40, 50};
delete values;
```

`int[]` representa un array dinámico con el campo `.data` y el método `.size()`.
También dispone de `.is_valid_idx(i)` para consultar un índice antes de
accederlo. Los arrays fijos ofrecen las mismas operaciones.
`resize(&ok) int[+n]{array, valores...}` agrega `n` posiciones y puede
inicializar algunas de ellas.

```rexon
int[3] fixed = {10, 20, 30};
@assert(fixed.size() == 3, "fixed size");
@assert(fixed.is_valid_idx(2) && !fixed.is_valid_idx(3), "fixed index");
```

## Builtins con `@`

Los builtins proporcionados por el runtime son:

```rexon
@print("valor = {}\n", value);
@assert(value > 0, "value debe ser positivo");
int converted = @cast(int, value);
@exit(1);

bool exists = @fexists("file.txt");
auto contents = @fread("file.txt");
auto written = @fwrite("file.txt", "hello");
auto appended = @fappend("file.txt", " world");
auto user_entry = @getline();

int status = @shell("echo hola");
@shell("bash", "echo ejecutado con bash");
```

`@shell` ejecuta un comando y devuelve su código de salida. Con un solo
argumento usa el intérprete predeterminado del sistema; con dos argumentos se
puede elegir `bash`, `zsh`, `cmd`, `powershell` o `pwsh`.

Las lambdas usan parámetros tipados y pueden indicar su retorno explícitamente:

```rexon
auto add = fn(int a, int b) int {
    return a + b;
};

auto log = fn(int code, char[] message, c_int flags) void {
    @print("{} {} {}\n", code, message, flags);
};
```

Capturan por referencia el entorno donde se crean y se convierten en lambdas
C++ durante la transpilación.

`@cpp("...")` inserta código C++ directamente en el archivo generado:

```rexon
@cpp("static_assert(sizeof(int) >= 4);");
```

Para incluir headers de C o C++ se puede usar `cinclude` en el nivel superior:

```rexon
cinclude "mi_header.h";
cinclude <vector>;
```

La primera forma genera `#include "mi_header.h"` y la segunda genera
`#include <vector>` en el C++ transpilado.

El formato de `@print` usa `{}` como marcador de posición. `@assert` lanza un
fallo durante `--test` y usa una aserción en una ejecución normal.

## `defer`

`defer` ejecuta una expresión o un bloque al salir del scope actual:

```rexon
fn work() {
    defer @print("fin\n");
    defer {
        @print("limpieza\n");
    }
    @print("trabajando\n");
}
```

## Imports y namespaces

Un archivo puede importar otro archivo `.rxn`:

```rexon
import math;
```

El compilador busca el módulo en el directorio actual, junto al archivo que lo
importa y en los directorios indicados con `-I`:

```sh
./rexonc -I lib app.rxn -o app
```

Los imports se procesan una sola vez y los ciclos se ignoran después de la
primera visita. También se pueden declarar namespaces:

```rexon
namespace app {
    fn version() int { return 1; }
}
```

### RexLib

El directorio `RexLib/` incluye módulos listos para importar con `-I`:

- `math`: operaciones numéricas, conversiones de ángulos y aritmética segura.
- `collections`: suma, producto, búsqueda y conteo para arrays de enteros.
- `char`: clasificación ASCII, conversión de mayúsculas/minúsculas y dígitos
  hexadecimales.
- `string`: longitud, comparación, prefijos, sufijos y búsqueda sobre slices
  `char#[]` sin depender de terminadores NUL.
- `memory`: operaciones explícitas sobre buffers de enteros y punteros.
- `graphics`: tipos `rexgfx::vec2`, `rect`, `circle` y `color`, además de
  distancia, producto punto, colisiones y mezcla de colores.
- `color`: colores RGBA predefinidos y composición alfa.
- `easing`: interpolaciones para animaciones y transiciones.

`math` también incluye `gcd`, `lcm`, `fibonacci`, `is_prime`,
`next_power_of_two` y `wrap`, todos con resultados opcionales cuando una
operación puede desbordar o no tiene sentido.

Los módulos `char`, `string` y `memory` no esconden la propiedad de memoria:
los punteros reservados con `new` deben liberarse con `delete` por el código que
los creó.

```rexon
import graphics;
rexgfx::vec2 player = rexgfx::vec(10.0, 4.0);
rexgfx::rect screen = rexgfx::make_rect(0.0, 0.0, 800.0, 600.0);
@assert(rexgfx::contains(screen, player), "player fuera de pantalla");
```

El runtime ofrece `@cwd()`, `@env(name)`, `@mkdir(path)`, `@remove(path)` y
`@sleep(milliseconds)` para programas que necesitan interactuar con su entorno.

## Tests

Declara tests con un nombre descriptivo:

```rexon
test "addition" {
    @assert(1 + 1 == 2, "addition");
}
```

Ejecuta todos los tests:

```sh
./rexonc --test tests/test.rxn
```

Ejecuta solo uno:

```sh
./rexonc --test --unique-test "addition" tests/test.rxn
```

## Opciones del compilador

```text
rexonc [-I dir] [-o output]
       [-c]
       [--check] [--version]
       [--test [--unique-test name]]
       [--emit-cpp] [--keep-temp] [--tokens]
       [--get-std-hpp]
       file.rxn [clang++ flags...]
```

- `-I dir`: agrega un directorio de búsqueda para imports y headers.
- `-c`: compila solamente a un archivo objeto (`.o` o `.obj`); sin `-o`, usa
  el nombre de entrada con extensión `.o`.
- `-o output`: define el nombre del ejecutable o del C++ generado. Si termina
  en `.o` o `.obj`, compila un archivo objeto en lugar de enlazar un ejecutable.
- `--test`: compila y ejecuta los bloques `test`.
- `--unique-test name`: ejecuta solo el test indicado.
- `--check`: transpila y valida todos los módulos sin invocar el compilador C++.
- `--version`: muestra la versión del compilador (`1.0.0`).
- `--emit-cpp`: genera C++ y no invoca `clang++`.
- `--keep-temp`: conserva los archivos temporales del runtime.
- `--tokens`: imprime los tokens lexados en `stderr`.
- `--get-std-hpp`: imprime el header estándar embebido y termina.

Los flags que no reconoce `rexonc` se pasan a `clang++`, por ejemplo:

```sh
./rexonc app.rxn -O0 -g -Wall -o app
```

## Ejemplos

El directorio [`tests/`](./tests/) contiene ejemplos ejecutables, incluyendo
imports, structs, opcionales, arrays, memoria dinámica, builtins y tests.
