# Every file that carries the application version. Dot-sourced by bump-version.ps1
# (writes them) and check-version.ps1 (verifies them), so both always agree on where
# the version lives. MSI reads vcpkg.json at build time (installer/urPDF.wixproj).
#
# Suffix is appended to the x.y.z version: MSIX needs four components.
$script:VersionSources = @(
    [pscustomobject]@{
        RelativePath = 'urPDF/Package.appxmanifest'
        Pattern      = '(?s)(?<prefix><Identity\b[^>]*?\bVersion=")(?<version>[^"]+)(?<suffix>")'
        Suffix       = '.0'
    }
    [pscustomobject]@{
        RelativePath = 'vcpkg.json'
        Pattern      = '(?<prefix>"version-string"\s*:\s*")(?<version>[^"]+)(?<suffix>")'
        Suffix       = ''
    }
    [pscustomobject]@{
        RelativePath = 'urPDF/AboutPage.xaml'
        Pattern      = '(?<prefix><TextBlock\b[^>]*?\bText="Version )(?<version>[0-9]+(?:\.[0-9]+){1,3})(?<suffix>")'
        Suffix       = ''
    }
)
