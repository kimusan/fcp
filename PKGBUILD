# Maintainer: Kim Schulz <kim@schulz.dk>

pkgname=fcp
_pkgname=fcp
pkgver=2.1.0
pkgrel=1
pkgdesc="Linux copy tool with progress, safety checks, and parallelism"
arch=('x86_64' 'aarch64')
url="https://github.com/kimusan/fcp"
license=('MIT')
depends=('openssl')
makedepends=('gcc')
source=("${pkgname}-${pkgver}.tar.gz::https://github.com/kimusan/fcp/archive/refs/tags/v${pkgver}.tar.gz")
sha256sums=('SKIP')

build() {
    cd "${pkgname}-${pkgver}"
    make
}

package() {
    cd "${pkgname}-${pkgver}"
    make DESTDIR="${pkgdir}" PREFIX="/usr" install
    install -Dm644 LICENSE "${pkgdir}/usr/share/licenses/${pkgname}/LICENSE"
}
