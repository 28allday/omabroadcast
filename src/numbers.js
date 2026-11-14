.pragma library

// What a number field lets through while typing: digits, a sign, a point or a comma.
var typing = /^[-+]?[0-9.,]*$/

// A number as someone typed it, NaN when it cannot be told for certain. A point
// always reads as the fields show numbers, whatever the language. A comma is the
// decimal point where the language says so; elsewhere it only separates thousands
// (12,000), and anywhere else it is a mistake, not a number to guess at.
function read(text, decimalPoint) {
    if (text === "") return NaN
    if (decimalPoint === ",") return Number(text.replace(",", "."))
    if (text.includes(",") && !/^[-+]?[0-9]{1,3}(,[0-9]{3})+([.][0-9]*)?$/.test(text)) return NaN
    return Number(text.replace(/,/g, ""))
}
