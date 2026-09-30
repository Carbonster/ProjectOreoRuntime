from setuptools import Distribution, setup


class WindowsBinaryDistribution(Distribution):
    def has_ext_modules(self):
        return True


setup(distclass=WindowsBinaryDistribution)
