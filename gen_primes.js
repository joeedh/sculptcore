
let max = 1 << 29;
let list = new Uint32Array(max)
s = '{'

for (let i = 0; i < max; i++) {
    list[i] = i;
}

let last_prime = 2;
let final_prime;

for (let i = 2; i < max; i++) {
    if (list[i] == 0) {
        continue;
    }

    for (let j = i + i; j < max; j += i) {
        list[j] = 0;
    }

    final_prime = i;

    if (i - last_prime > last_prime * 0.25) {
        s += `  ${i},`;
        last_prime = i;
    }
}

s += `  ${final_prime},`;
s += "\n}";
console.log(s)