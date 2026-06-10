package ai.edgez.server.lwm2m.wasm;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;

import org.eclipse.aether.RepositorySystem;
import org.eclipse.aether.RepositorySystemSession;
import org.eclipse.aether.artifact.Artifact;
import org.eclipse.aether.artifact.DefaultArtifact;
import org.eclipse.aether.connector.basic.BasicRepositoryConnectorFactory;
import org.eclipse.aether.repository.LocalRepository;
import org.eclipse.aether.repository.RemoteRepository;
import org.eclipse.aether.repository.RepositoryPolicy;
import org.eclipse.aether.resolution.ArtifactRequest;
import org.eclipse.aether.resolution.ArtifactResult;
import org.eclipse.aether.spi.connector.RepositoryConnectorFactory;
import org.eclipse.aether.spi.connector.transport.TransporterFactory;
import org.eclipse.aether.transport.http.HttpTransporterFactory;
import org.eclipse.aether.DefaultRepositorySystemSession;
import org.eclipse.aether.impl.DefaultServiceLocator;
import org.apache.maven.repository.internal.MavenRepositorySystemUtils;

/**
 * Minimal Maven Resolver helper to pull a classifier WASM artifact at runtime.
 */
final class MavenWasmResolver {

    Path resolve(String groupId,
                 String artifactId,
                 String version,
                 String classifier,
                 String extension,
                 String repoUrl,
                 Path localRepo) throws Exception {

        Files.createDirectories(localRepo);

        RepositorySystem system = newRepositorySystem();
        RepositorySystemSession session = newSession(system, localRepo);

        Artifact artifact = new DefaultArtifact(groupId, artifactId, classifier, extension, version);
        ArtifactRequest request = new ArtifactRequest();
        request.setArtifact(artifact);
        if (repoUrl != null && !repoUrl.isBlank()) {
            RemoteRepository remote = new RemoteRepository.Builder("wasm-remote", "default", repoUrl)
                .setSnapshotPolicy(new RepositoryPolicy(true, RepositoryPolicy.UPDATE_POLICY_ALWAYS, RepositoryPolicy.CHECKSUM_POLICY_WARN))
                .setReleasePolicy(new RepositoryPolicy(true, RepositoryPolicy.UPDATE_POLICY_DAILY, RepositoryPolicy.CHECKSUM_POLICY_WARN))
                .build();
            request.setRepositories(List.of(remote));
        } else {
            request.setRepositories(List.of()); // Resolve from local repo only when no remote is provided.
        }

        ArtifactResult result = system.resolveArtifact(session, request);
        return result.getArtifact().getFile().toPath();
    }

    private RepositorySystem newRepositorySystem() {
        DefaultServiceLocator locator = MavenRepositorySystemUtils.newServiceLocator();
        locator.addService(RepositoryConnectorFactory.class, BasicRepositoryConnectorFactory.class);
        locator.addService(TransporterFactory.class, HttpTransporterFactory.class);
        return locator.getService(RepositorySystem.class);
    }

    private RepositorySystemSession newSession(RepositorySystem system, Path localRepoPath) {
        DefaultRepositorySystemSession session = MavenRepositorySystemUtils.newSession();
        LocalRepository localRepo = new LocalRepository(localRepoPath.toFile());
        session.setLocalRepositoryManager(system.newLocalRepositoryManager(session, localRepo));
        return session;
    }
}
