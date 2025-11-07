.. _connection:

Connection URL and Connection String
************************************

When opening a VDS its possible to use a URL and a connection string. The
purpose of this is to create a unified connection API that can be used for
multiple IO backends.

The URL consist ``protocol://resource/sub_path``. The protocol will define the
backend to be used. Currently this can be:
  - s3
  - azure
  - gs
  - sd
  - azureSAS
  - file

If the file protocol scheme is used the rest of the path will be decoded according to url encoding, however
if OpenVDS fails to recognise the protocol of a url it will treat the url as a local file.

For ``s3`` and ``gs`` the ``resource`` will be the bucket while for ``azure`` the ``resource``
will be the container. ``azureSAS`` will treat ``resource/sub_path`` as the baseURL
of the SAS url and prepend ``https``.

The ``connection`` string will provide ``protocol`` or backend specific data for
the connection to the VDS. The string is a key/value string where the key and
value is separated with a ``=``. Each key/value pair is separated with a ``;``.
It is allowed for the value to contain extra ``=``, but the key can not. Having
duplicate keys is NOT allowed, and will cause an error.

There are some globally recognized keys for the connection string.
They are:
  - WaveletAdaptiveTolerance
  - WaveletAdaptiveRatio

``WaveletAdaptiveTolerance`` and ``WaveletAdaptiveRatio`` are mutually exclusive.

The recognised keys for ``s3`` connection string are

  - Region
  - EndpointOverride
  - AccessKeyId or AwsAccessKeyId
  - SecretKey, SecretAccessKey, AwsSecretKey or AwsSecretAccessKey
  - SessionToken
  - Expiration (Note: AWS SDK only accepts 'Z' timezone for ISO 8601 time)
  - LogFilenamePrefix
  - LogLevel (Note: this is to control the AWS specific logs. Possible values: Off, Fatal, Error, Warn, Info, Debug, Trace)
  - ConnectionTimoutMs
  - RequestTimeoutMs
  - DisableInitApi (This will disable init/deinit of AWS sdk. Possible values: 1, On, True, Yes (case insensitive), all other values are false.)

Note: Key values for S3 can be specified in `CamelCase` or `snake_case`


``azure`` has two modes. The connection string mode or the bearer token mode.
If the connection string contains a key with name BearerToken it will parse the
string and extract following parameters:
  - AccountName
  - BearerToken
Both parameters are mandatory.

In the connection string mode the connection string is parsed according to the rules described here:
https://docs.microsoft.com/en-us/azure/storage/common/storage-configure-connection-string

``gs`` will accept the following keys

  - Token
  - CredentialsFile
  - JsonCredentials
  - SignedUrl (Possible values: True, Yes, On, False, No, Off. Default value: False, No, Off)
The Token is the OAuth2 token.

``sd`` will accept the following keys

  - SdAuthorityUrl
  - SdApiKey
  - SdToken
  - LogLevel
  - AlreadyRegistered
  - LegalTag

The AlreadyRegistered option is a way to force OpenVDS to overwrite an existing
dataset. It only works with the new DmsIOManager and not the depreacted sdapi
based backend.

The token `SdToken` is typically a short lived token. The sd IO backend also
supports using a `RefreshToken` flow. Then its not needed to specify the sdtoken
but following key/value pairs have to specified:

  - AuthTokenUrl
  - ClientId
  - ClientSecret (Optional)
  - RefreshToken
  - Scopes  (Optional)
    
When these paramaters are provided OpenVDS will send a "x-www-form-urlencoded"
http request to `AuthTokenUrl` with supplied values and also added:
grant_type=refresh_token&scope=openid email.
scope can be overrided with the Scopes connectionString parameter.
The ClientSecret is optional, as it is not needed when using, by eg, OAuth2 + PKCE.
The used refresh token will be updated If the response contains a refresh_token member.
It is the access_token member of the return that will be used as the sdtoken until it expires.

Alternatively, for Azure-hosted deployments (Azure VMs, Azure Container Instances, 
AKS, etc.), you can use `MsiEnable` instead of providing an explicit SdToken or 
using RefreshToken flow. When `MsiEnable` is set to true (or yes, on, 1), OpenVDS 
will automatically fetch an access token from Azure's Instance Metadata Service 
(IMDS) using Managed Identity. This only works when running on Azure infrastructure 
with Managed Identity configured. Key values can use `snake_case` (msi_enable) or 
`CamelCase` (MsiEnable).

NOTE: Key values for `sd` can use `snake_case` instead of `CammelCase` to match the
look and feel of the response from the token provider.

If the seismic dms service is targeting AWS, please make sure to set the
AWS_REGION environment variable to the correct region.

``azureSAS`` will accept the following keys

  - Suffix
The suffix is the SAS token.

Some examples:

S3::

  url = "s3://my_bucket/somepath"
  connection = "Region=eu-north-1"
  #note that if Region is empty then the backend will try and find the region, but this only works if the credentials are the bucket owner

Azure::

  url = "azure://my_container/somepath"
  connection = "DefaultEndpointsProtocol=https;AccountName=developer;AccountKey=somekey;EndpointSuffix=core.windows.net"

GS::

  url = "gs://my_bucket/somepath"
  connection = "Token=some_token"

SD::

  url = "sd://tenent/subprojet/subfolder"
  connection = "sdauthorityurl=https://some_osdu.instance.com/seistore-svc/api/v3;sdapikey=ABC;sdtoken=some_token"
  
  # Or using Azure Managed Identity (for Azure-hosted deployments):
  connection = "sdauthorityurl=https://some_osdu.instance.com/seistore-svc/api/v3;sdapikey=ABC;msienable=true"

AzureSAS::

  url = "azureSAS://storageAccount.blob.core.windows.net/container/subpath"
  connection = "Suffix=?sv=2019-10-10&ss=bfqt&srt=sco&sp=rwdlacupx&se=2020-06-11T18:26:15Z&st=2020-06-11T10:26:15Z&spr=https&sig=V5glday54BztU8qtiIlRjEOnboiy4Y%2Fu%2FhbqRqWSN2E%3D"

