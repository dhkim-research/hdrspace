#ifdef NANOGUI_PYTHON

#include "python.h"
#include <nanogui/ema.h>

void register_ema(nb::module_ &m) {
    nb::class_<EMA>(m, "EMA", D(EMA))
        .def(nb::init<float>(), "weight"_a = 0.983f, D(EMA, EMA))
        .def("reset", &EMA::reset, D(EMA, reset))
        .def("put", &EMA::put, "sample"_a, D(EMA, put))
        .def("value", &EMA::value, D(EMA, value))
        .def("weight", &EMA::weight, D(EMA, weight))
        .def("sample_count", &EMA::sample_count, D(EMA, sample_count));
}

#endif