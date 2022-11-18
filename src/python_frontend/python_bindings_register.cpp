#include <pybind11/pybind11.h>
#include <python_frontend/python_bindings.h>

PYBIND11_MODULE(nvfuser, m) {
    m.doc() = "nvfuser python API"; // optional module docstring
    torch::jit::initNvFuserPythonBindings(m.ptr());
}
